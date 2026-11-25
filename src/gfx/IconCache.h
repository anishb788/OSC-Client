#pragma once
// Real app icons (Increment 11). MAIN THREAD ONLY, except the jobs it submits (IconCache.cpp), which only touch a
// shared_ptr<Shared> and never the cache, the renderer or any SDL texture.
//
// FLOW
//   drawAppIcon -> get(slug, url, size, now): a texture + fade alpha, or nullopt (draw the placeholder).
//   A miss enqueues ONE request for that slug, which is two worker jobs in a row. (1) An IconLocal job (its own lane,
//   always served before downloads, newest first) reads <cacheDir>/icons/<slug>.png if its size equals the catalog's
//   iconSize and decodes it. Only on a miss it posts back to the main thread, which submits (2) an Icon-class job (one
//   at a time, newest first): GET the URL (64 KB cap, 4 s idle / 8 s total), then validate and decode the PNG into a CPU
//   SDL_Surface (IconPng.h: chunk walk and size limits first) and post it. A cached icon therefore never waits behind a
//   stalled download. A freshly fetched icon is then written to the SD card by a Background job (at most 1 MB queued, then
//   writes are skipped), so showing icons always ranks above SD writes. The main thread turns at most kUploadsPerFrame
//   surfaces per frame into SDL_Textures (the only place textures are created) and keeps them in an LRU.
//
// THE STATE MACHINE, per slug (an Entry): none -> in flight (ticket) -> texture | failed.
//   - texture: in the LRU (<= maxTextures and <= maxTextureBytes; the least recently drawn is destroyed first). It
//     remembers the iconSize it was loaded for; a different size in the catalog starts a refetch while the old texture
//     keeps showing.
//   - failed: remembered so a missing icon is not requested every frame. A transient failure (no network, timeout, a
//     5xx) may retry after kRetrySec, up to kMaxAttempts tries in all; anything else (404, not a PNG, too big) is final
//     for the session. A Refresh (a new catalog generation) clears every failure, so it is tried once more.
//   - in flight: carries a ticket, and a result is accepted only if its ticket is still the entry's. A request that is
//     dropped (below) or an entry that moved on makes the late result stale; it is freed unused.
//
// DROPPING: a request is "wanted" while get() keeps being called for it. endFrame() cancels the jobs (queued or RUNNING)
// whose slug has not been asked for in kStaleFrames frames: paging a list, leaving a screen or a Refresh makes the old
// icons unwanted, and the newest-first order already serves the new page first. A running download is cancelled too,
// because it holds the one Icon slot and may be stalled; the worker's step budget makes it stop within ~50 ms. Its
// ticket is cleared at once, so the job's Dropped result is recognised as stale.
//
// SHUTDOWN: shutdown() frees the textures (call it before the renderer goes). Surfaces posted but never drawn are
// freed with their closures when the worker discards its results.
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/FileUtil.h"

struct SDL_Renderer;
struct SDL_Texture;
struct IconResult;
namespace net { class Worker; }

struct IconCacheConfig {
	// Mock mode and tests without a renderer: get() always returns nullopt and nothing is requested.
	bool enabled = true;
	// <dataDir>/cache; empty = no SD cache (every icon is fetched, nothing is written).
	std::string cacheDir;
	// false (the `offline` dev option): SD only. A miss is a final failure for the session, and a size mismatch is
	// accepted (a stale icon beats a placeholder when nothing can be fetched).
	bool network = true;
	// Dev option `icons=<base>`: icons are fetched from "<base>/<slug>/icon.png" instead of the catalog's URL.
	std::string urlBase;

	size_t maxTextures = 128;            // epic: 128 icons, about 3 MB of RGBA at 128x48
	size_t maxTextureBytes = 4u << 20;   // the same bound in bytes, for a catalog with bigger icons
	int uploadsPerFrame = 4;             // texture creations per frame, so a burst of arrivals never hitches a frame
	// Icons are 2 to 14 KB and the cap is 64 KB. Idle 4 s: a server that sends nothing for that long is not going to; total
	// 8 s: even a trickle must deliver 64 KB at 8 KB/s. Together they bound how long one dead download holds the single
	// Icon slot (it used to be 15 s), and a download nobody wants any more is cancelled outright (see DROPPING).
	int fetchTimeoutMs = 8000;           // total wall clock for one GET
	int fetchIdleMs = 4000;              // no bytes for this long
	size_t maxFileBytes = 64 * 1024;     // network body and SD file cap (iconpng::kMaxFileBytes)
	uint32_t staleFrames = 8;            // frames without get() before a request is cancelled, queued or running
	double fadeSec = 0.20;               // fade-in over the placeholder
	double retrySec = 20;                // wait before retrying a transient failure
	int maxAttempts = 3;                 // tries per slug and generation for transient failures
	FileOps fileOps;                     // the SD writes' filesystem calls; tests inject failures
};

class IconCache {
public:
	struct View {
		SDL_Texture* tex = nullptr;
		int w = 0, h = 0;
		float alpha = 1;   // 0..1, fading in over the placeholder
	};
	struct Stats {
		size_t textures = 0, textureBytes = 0;
		uint64_t evictions = 0, requested = 0, fromSd = 0, fetched = 0, failed = 0, dropped = 0, stale = 0, uploaded = 0;
		size_t inFlight = 0;
		uint64_t writesSkipped = 0;   // fetched icons not written to the SD card because too many writes were queued
	};

	// `worker` may be null or not running: requests then fail at once, as final failures. `renderer` may be null only
	// when the cache is disabled or the test never lets a result arrive.
	IconCache(net::Worker* worker, SDL_Renderer* renderer, IconCacheConfig cfg);
	~IconCache();
	IconCache(const IconCache&) = delete;
	IconCache& operator=(const IconCache&) = delete;

	// Once per frame before anything is drawn: handles finished jobs (at most uploadsPerFrame textures are created),
	// and notices a new catalog generation (clears the failed set). `now` is seconds on any steady clock (Ui::time).
	void beginFrame(double now, uint32_t catalogGeneration);
	// The texture to draw for `slug`, or nullopt for the placeholder. Enqueues a job on a miss. The texture stays valid
	// until the next beginFrame(). `size` is the catalog's assets.icon.size (0 = unknown).
	std::optional<View> get(std::string_view slug, std::string_view url, uint64_t size, double now);
	// Once per frame after drawing: drops queued requests that are no longer asked for, and logs stats now and then.
	void endFrame();

	// Startup pruning: a Background job deletes icons/*.png of slugs not in `keep`, a few files per step and for at most
	// a few seconds. An empty `keep` does nothing (a broken catalog must not wipe the cache). Runs once per cache.
	void startPrune(const std::set<std::string>& keep);

	void shutdown();
	Stats stats() const;
	bool enabled() const { return cfg_.enabled; }
	const IconCacheConfig& config() const { return cfg_; }

	// Defined in IconCache.cpp; public only so that file's helpers can name them.
	struct Shared;
	struct Entry;

private:
	struct KeyHash {
		using is_transparent = void;
		size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
	};
	using Map = std::unordered_map<std::string, Entry, KeyHash, std::equal_to<>>;

	void handleResult(IconResult& r, double now);
	bool upload(Entry& e, IconResult& r);
	void markFailed(Entry& e, bool permanent, double now);
	bool mayRequest(Entry& e, double now);
	void enqueue(Entry& e, std::string_view url, uint64_t size, double now);
	void touch(Entry& e);
	void dropTexture(Entry& e);
	void evict();
	void dropStale();
	void eraseIfIdle(Entry& e);

	net::Worker* worker_;
	SDL_Renderer* renderer_;
	IconCacheConfig cfg_;
	std::shared_ptr<Shared> shared_;
	std::unique_ptr<Map> map_;
	std::list<Entry*> lru_;   // most recently drawn first
	size_t textureBytes_ = 0;
	size_t inFlight_ = 0;
	uint64_t nextTicket_ = 0;
	uint32_t frame_ = 0, gen_ = 0, lastLogFrame_ = 0;
	bool pruned_ = false, workerWarned_ = false;
	Stats stats_;
	Stats lastLogged_;
};
