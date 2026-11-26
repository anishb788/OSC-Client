#include "gfx/IconCache.h"

#include <SDL_render.h>
#include <SDL_surface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <unordered_set>
#include <vector>

#include "catalog/Catalog.h"
#include "core/Log.h"
#include "core/Time.h"
#include "gfx/IconPng.h"
#include "gfx/IconStore.h"
#include "net/Http.h"
#include "net/HttpTransfer.h"
#include "net/Worker.h"

namespace {
using Clock = std::chrono::steady_clock;
using timeutil::msSince;
constexpr const char* kTagPrefix = "icon:";
}  // namespace

// What one icon request needs, copied at enqueue time (no AppEntry, no cache pointer). Not in an anonymous namespace
// only because IconResult carries one; this file is the only translation unit that names it.
struct IconSpec {
	std::string slug, url, cacheDir;
	uint64_t size = 0, ticket = 0;
	bool network = true;
	int totalTimeoutMs = 8000, idleTimeoutMs = 4000;
	size_t maxBytes = iconpng::kMaxFileBytes;
	FileOps ops;
};

// What a job tells the main thread. Posted as a closure, so a result nobody drains is freed with the closure.
struct IconResult {
	// Miss: the SD job found no usable file and the icon needs the network; the main thread submits that job (spec).
	enum class Outcome { Ok, Miss, Dropped, Transient, Permanent };
	std::string slug;
	uint64_t ticket = 0;
	uint64_t size = 0;   // the catalog size this job was started for
	Outcome outcome = Outcome::Permanent;
	bool fromSd = false;   // Ok only: read from the SD cache rather than downloaded
	iconpng::SurfacePtr surface;
	std::string why;
	std::shared_ptr<IconSpec> spec;   // Miss only
};

// Touched on the main thread only, except the atomics (the worker's write jobs). Jobs keep it alive with a shared_ptr, so
// a job that outlives the cache (the worker is stopped after it, or its closure is discarded later) is harmless.
struct IconCache::Shared {
	std::deque<IconResult> inbox;
	std::atomic<int> writeFails{0};
	std::atomic<size_t> writeQueued{0};   // bytes held by IconWriteJobs that have not been destroyed yet
	std::atomic<uint32_t> writesSkipped{0};   // icons shown but not written because writeQueued was at its cap
};

struct IconCache::Entry {
	std::string slug;
	SDL_Texture* tex = nullptr;
	int w = 0, h = 0;
	size_t bytes = 0;
	uint64_t texSize = 0;       // the catalog size the texture was loaded for
	double shownAt = -1;        // when get() first returned the texture (the fade-in starts then)
	uint32_t reusedGen = 0;     // the generation a reuse across a Refresh was last logged for
	std::list<Entry*>::iterator lruPos;
	uint64_t ticket = 0;        // the job in flight, 0 = none
	uint32_t lastReq = 0;       // frame of the last get()
	bool failed = false, permanent = false;
	double retryAt = 0;
	int attempts = 0;
};

namespace {

using Shared = IconCache::Shared;

// A write that fails this often in a row (read-only or full card) stops being attempted, with one log line.
constexpr int kMaxWriteFails = 3;
// Writes are Background class, so a long Icon queue (or a slow card) keeps them waiting, and each one holds its PNG in
// memory. Past this many queued bytes new icons are shown but not written; they are fetched again on a later launch.
constexpr size_t kMaxQueuedWriteBytes = 1u << 20;

// Writes a freshly fetched icon to the SD card. Background class: it only runs when no icon needs the worker, and at
// least every Worker::kBackgroundSliceMs otherwise. The bytes were validated as a PNG before they got here.
class IconWriteJob : public net::Job {
public:
	IconWriteJob(std::string dir, std::string slug, std::shared_ptr<const std::string> bytes, FileOps ops, std::shared_ptr<Shared> sh)
		: dir_(std::move(dir)), slug_(std::move(slug)), bytes_(std::move(bytes)), ops_(ops), sh_(std::move(sh)) {}
	// The caller added bytes_->size() to writeQueued when it created the job; whichever way the job ends (written,
	// skipped, or destroyed unrun when the worker stops) this releases it.
	~IconWriteJob() override { sh_->writeQueued.fetch_sub(bytes_->size()); }
	net::JobStep step(net::JobContext&) override {
		if (sh_->writeFails.load() >= kMaxWriteFails) return net::JobStep::Done;
		// Runs to the end even when cancelled: one small file, and a stop() must not leave a half-written tmp.
		const auto t0 = Clock::now();
		if (iconstore::write(dir_, slug_, *bytes_, ops_)) {
			sh_->writeFails.store(0);
			LOG("Icon %s: wrote %zu bytes to the SD cache in %.0f ms", slug_.c_str(), bytes_->size(), msSince(t0));
		} else if (sh_->writeFails.fetch_add(1) + 1 == kMaxWriteFails) {
			LOG("Icon cache: %d SD writes failed in a row (read-only or full card?), no more icons are written this session", kMaxWriteFails);
		} else {
			LOG("Icon %s: SD write failed", slug_.c_str());
		}
		return net::JobStep::Done;
	}

private:
	std::string dir_, slug_;
	std::shared_ptr<const std::string> bytes_;
	FileOps ops_;
	std::shared_ptr<Shared> sh_;
};

// Deletes the icons of slugs that left the catalog, 8 files per step (a step is one non-sliceable chunk of SD work), and
// gives up after kMaxMs so a card full of leftovers never keeps the worker busy; the next launch continues. The first
// step only lists the directory (Pruner), so even that is a step of its own.
//
// `keep` is the catalog the app has now. With the dev option `api=<url>` pointing at a server that lists only a few apps,
// this deletes every other cached icon. That is intended: the cache mirrors the catalog in use, and a real catalog
// refetches whatever it needs.
class IconPruneJob : public net::Job {
public:
	static constexpr size_t kPerStep = 8;
	static constexpr double kMaxMs = 3000;
	IconPruneJob(std::string dir, const std::set<std::string>& keep) : pruner_(std::move(dir), keep), t0_(Clock::now()) {}
	net::JobStep step(net::JobContext& ctx) override {
		const bool first = !snapshotLogged_;
		const bool more = pruner_.step(kPerStep);
		if (first) {
			snapshotLogged_ = true;
			LOG("Icon cache prune: directory snapshot of %zu files took %.0f ms", pruner_.scanned(), pruner_.snapshotMs());
		}
		if (more && !ctx.cancelled() && msSince(t0_) < kMaxMs) return net::JobStep::Pending;
		LOG("Icon cache prune: %zu files looked at, %zu removed in %.0f ms%s", pruner_.scanned(), pruner_.removed(), msSince(t0_),
		    more ? " (stopped early, the rest goes next launch)" : "");
		return net::JobStep::Done;
	}

private:
	iconstore::Pruner pruner_;
	Clock::time_point t0_;
	bool snapshotLogged_ = false;
};

// Posts the job's outcome. The closure runs on the main thread (drainResults) and holds the Shared state, never the cache.
void postResult(net::JobContext& ctx, const std::shared_ptr<Shared>& sh, const IconSpec& spec, IconResult::Outcome o, std::string why,
                bool fromSd = false, iconpng::SurfacePtr surface = nullptr, std::shared_ptr<IconSpec> miss = nullptr) {
	auto res = std::make_shared<IconResult>();
	res->slug = spec.slug;
	res->ticket = spec.ticket;
	res->size = spec.size;
	res->outcome = o;
	res->fromSd = fromSd;
	res->why = std::move(why);
	res->surface = std::move(surface);
	res->spec = std::move(miss);
	ctx.post([sh, res] { sh->inbox.push_back(std::move(*res)); });
}

// Step 1 of an icon, in the IconLocal lane: the SD card. A hit is decoded and posted; a miss posts Miss and the main thread
// submits the network job. Two jobs instead of one because the Icon class has a single slot: a cached icon must never
// wait behind a download that is stalled until its timeout. The hop through the main thread (instead of submitting from
// here) keeps the ticket logic in one place: a request dropped meanwhile makes the Miss stale and no download starts.
class IconSdJob : public net::Job {
public:
	IconSdJob(IconSpec spec, std::shared_ptr<Shared> sh) : spec_(std::move(spec)), sh_(std::move(sh)) {}

	net::JobStep step(net::JobContext& ctx) override {
		if (ctx.cancelled()) {
			postResult(ctx, sh_, spec_, IconResult::Outcome::Dropped, {});
		} else if (readCache()) {
			postResult(ctx, sh_, spec_, IconResult::Outcome::Ok, {}, true, std::move(surface_));
		} else if (!spec_.network) {
			postResult(ctx, sh_, spec_, IconResult::Outcome::Permanent, "not in the SD cache (offline)");
		} else {
			postResult(ctx, sh_, spec_, IconResult::Outcome::Miss, {}, false, nullptr, std::make_shared<IconSpec>(spec_));
		}
		return net::JobStep::Done;
	}

private:
	// True when a valid cached icon was decoded into surface_.
	bool readCache() {
		const auto t0 = Clock::now();
		std::string bytes;
		// Offline accepts any size: a stale icon beats a placeholder when nothing can be fetched.
		const auto rs = iconstore::read(spec_.cacheDir, spec_.slug, spec_.network ? spec_.size : 0, spec_.maxBytes, bytes);
		switch (rs) {
		case iconstore::Read::Missing:
		case iconstore::Read::BadSlug: return false;   // BadSlug cannot happen: get() validates the slug first
		case iconstore::Read::SizeMismatch:
			LOG("Icon %s: cached file is not the catalog's size (%llu), fetching again", spec_.slug.c_str(), (unsigned long long)spec_.size);
			return false;
		case iconstore::Read::IoError:
			LOG("Icon %s: SD read error, leaving the file alone", spec_.slug.c_str());
			return false;
		case iconstore::Read::TooLarge:
			LOG("Icon %s: cached file is over %zu bytes, deleting it", spec_.slug.c_str(), spec_.maxBytes);
			break;
		case iconstore::Read::Ok: {
			const double readMs = msSince(t0);
			const auto t1 = Clock::now();
			const auto v = iconpng::decode(bytes, surface_);
			if (v == iconpng::Verdict::Ok) {
				LOG("Icon %s: SD cache hit, %zu bytes, read %.0f ms, decode %.1f ms", spec_.slug.c_str(), bytes.size(), readMs, msSince(t1));
				return true;
			}
			LOG("Icon %s: cached file is bad (%s), deleting it", spec_.slug.c_str(), iconpng::name(v));
			break;
		}
		}
		// A corrupt or oversized file: it would fail every launch. Delete it and fall through to the network.
		iconstore::remove(spec_.cacheDir, spec_.slug);
		return false;
	}

	IconSpec spec_;
	std::shared_ptr<Shared> sh_;
	iconpng::SurfacePtr surface_;
};

// Step 2, Icon class (one slot): the download, then validate and decode, then hand the SD write to a Background job.
class IconNetJob : public net::Job {
public:
	IconNetJob(IconSpec spec, std::shared_ptr<Shared> sh) : spec_(std::move(spec)), sh_(std::move(sh)) {}

	net::JobStep step(net::JobContext& ctx) override {
		if (ctx.cancelled()) return finish(ctx, IconResult::Outcome::Dropped, {});
		if (!xfer_) start(ctx);
		// A cancel is noticed at the next step: a step is at most the 30 ms budget and a socket wait at most 50 ms.
		if (xfer_->step(ctx.budget) == HttpTransfer::StepResult::Pending) return net::JobStep::Pending;
		return fetched(ctx);
	}

private:
	void start(net::JobContext& ctx) {
		HttpRequest req;
		req.url = spec_.url;
		req.maxBody = spec_.maxBytes;   // the sink aborts at once, an oversized body is never buffered
		req.totalTimeoutMs = spec_.totalTimeoutMs;
		req.idleTimeoutMs = spec_.idleTimeoutMs;
		req.headers.emplace_back("Accept", "image/png");
		sink_ = std::make_unique<MemorySink>(req.maxBody);
		fetchStart_ = Clock::now();
		xfer_ = std::make_unique<HttpTransfer>(req, *sink_, nullptr, ctx.cancel, ctx.transport, &ctx.hosts);
	}

	net::JobStep fetched(net::JobContext& ctx) {
		const HttpResult r = xfer_->result();
		const bool ok = r.ok() && sink_->finishedOk();
		const double getMs = msSince(fetchStart_);
		if (r.error == HttpError::Cancelled || ctx.cancelled()) return finish(ctx, IconResult::Outcome::Dropped, {});
		if (!ok) {
			// 5xx and connection trouble may pass; a 404, an oversized body or a redirect loop will not.
			const bool transient = r.error == HttpError::Dns || r.error == HttpError::Connect || r.error == HttpError::Timeout ||
			                       r.error == HttpError::NoNetwork || r.error == HttpError::Io ||
			                       (r.error == HttpError::HttpStatus && r.status >= 500);
			const std::string why = describe(r);
			LOG("Icon %s: GET failed after %.0f ms: %s%s%s", spec_.slug.c_str(), getMs, why.c_str(), r.detail.empty() ? "" : " | ", r.detail.c_str());
			return finish(ctx, transient ? IconResult::Outcome::Transient : IconResult::Outcome::Permanent, why);
		}
		const auto t1 = Clock::now();
		const auto v = iconpng::decode(sink_->body(), surface_);
		if (v != iconpng::Verdict::Ok) {
			LOG("Icon %s: the download is not a usable PNG (%s), %zu bytes", spec_.slug.c_str(), iconpng::name(v), sink_->body().size());
			return finish(ctx, IconResult::Outcome::Permanent, std::string("bad icon: ") + iconpng::name(v));
		}
		LOG("Icon %s: GET ok in %.0f ms, %zu bytes, decode %.1f ms", spec_.slug.c_str(), getMs, sink_->body().size(), msSince(t1));
		if (sink_->body().size() != spec_.size && spec_.size > 0)
			LOG("Icon %s: the server sent %zu bytes, the catalog says %llu (it will be fetched again next launch)", spec_.slug.c_str(),
			    sink_->body().size(), (unsigned long long)spec_.size);
		// The picture goes to the screen first. The SD write is a separate Background job, so it never delays an icon.
		if (!spec_.cacheDir.empty()) {
			const size_t n = sink_->body().size();
			if (sh_->writeQueued.load() + n > kMaxQueuedWriteBytes) {
				sh_->writesSkipped.fetch_add(1);
				LOG("Icon %s: %zu KB of SD writes are already waiting, not writing this one (it is fetched again next launch)",
				    spec_.slug.c_str(), sh_->writeQueued.load() / 1024);
			} else {
				sh_->writeQueued.fetch_add(n);
				auto bytes = std::make_shared<const std::string>(std::move(sink_->body()));
				if (!ctx.worker.submit(net::JobClass::Background, std::string("iconw:") + spec_.slug,
				                       std::make_unique<IconWriteJob>(spec_.cacheDir, spec_.slug, std::move(bytes), spec_.ops, sh_)))
					LOG("Icon %s: the SD write was rejected (worker stopping)", spec_.slug.c_str());
			}
		}
		return finish(ctx, IconResult::Outcome::Ok, {}, true);
	}

	net::JobStep finish(net::JobContext& ctx, IconResult::Outcome o, std::string why, bool withSurface = false) {
		postResult(ctx, sh_, spec_, o, std::move(why), false, withSurface ? std::move(surface_) : nullptr);
		return net::JobStep::Done;
	}

	IconSpec spec_;
	std::shared_ptr<Shared> sh_;
	iconpng::SurfacePtr surface_;
	Clock::time_point fetchStart_;
	// sink_ before xfer_: an abandoned transfer's destructor still calls sink_->onFinish.
	std::unique_ptr<MemorySink> sink_;
	std::unique_ptr<HttpTransfer> xfer_;
};

}  // namespace

IconCache::IconCache(net::Worker* worker, SDL_Renderer* renderer, IconCacheConfig cfg)
	: worker_(worker), renderer_(renderer), cfg_(std::move(cfg)), shared_(std::make_shared<Shared>()), map_(std::make_unique<Map>()) {}

IconCache::~IconCache() { shutdown(); }

void IconCache::shutdown() {
	for (Entry* e : lru_) {
		if (e->tex) SDL_DestroyTexture(e->tex);
		e->tex = nullptr;
	}
	lru_.clear();
	textureBytes_ = 0;
	map_->clear();
	inFlight_ = 0;
	shared_->inbox.clear();
	cfg_.enabled = false;
}

IconCache::Stats IconCache::stats() const {
	Stats s = stats_;
	s.textures = lru_.size();
	s.textureBytes = textureBytes_;
	s.inFlight = inFlight_;
	s.writesSkipped = shared_->writesSkipped.load();
	return s;
}

void IconCache::beginFrame(double now, uint32_t generation) {
	frame_++;
	if (!cfg_.enabled) return;

	if (generation != gen_) {
		// A Refresh: the user asked to try again, so every failure is forgotten. Textures stay (keyed by slug, they hold no
		// catalog entries); get() checks them against the new catalog's sizes.
		size_t cleared = 0;
		for (auto it = map_->begin(); it != map_->end();) {
			Entry& e = it->second;
			if (e.failed) {
				cleared++;
				e.failed = e.permanent = false;
				e.attempts = 0;
				if (!e.tex && !e.ticket) { it = map_->erase(it); continue; }
			}
			++it;
		}
		if (gen_ != 0) LOG("IconCache: catalog generation %u, %zu textures kept (no download needed), %zu failed icons will be tried again",
		                   generation, lru_.size(), cleared);
		gen_ = generation;
	}

	// Finished jobs. Failures and drops are bookkeeping and always handled; textures are limited per frame.
	int uploads = 0;
	auto& inbox = shared_->inbox;
	while (!inbox.empty()) {
		if (inbox.front().outcome == IconResult::Outcome::Ok && uploads >= cfg_.uploadsPerFrame) break;
		IconResult r = std::move(inbox.front());
		inbox.pop_front();
		if (r.outcome == IconResult::Outcome::Ok) uploads++;
		handleResult(r, now);
	}
}

void IconCache::handleResult(IconResult& r, double now) {
	auto it = map_->find(r.slug);
	if (it == map_->end() || it->second.ticket != r.ticket) {
		stats_.stale++;   // dropped, replaced or cleared since: nobody is waiting for this
		return;
	}
	Entry& e = it->second;
	if (r.outcome == IconResult::Outcome::Miss) {
		// Still wanted (the ticket matches) and not on the SD card: the same request continues as a download. The ticket
		// and the in-flight count carry over; dropStale() cancels this job by its tag like the SD job before it.
		const IconSpec& sp = *r.spec;
		if (!worker_ || !worker_->submit(net::JobClass::Icon, kTagPrefix + e.slug, std::make_unique<IconNetJob>(sp, shared_))) {
			e.ticket = 0;
			if (inFlight_) inFlight_--;
			markFailed(e, true, now);
			eraseIfIdle(e);
		}
		return;
	}
	e.ticket = 0;
	if (inFlight_) inFlight_--;
	switch (r.outcome) {
	case IconResult::Outcome::Miss: break;   // handled above
	case IconResult::Outcome::Ok:
		if (upload(e, r)) (r.fromSd ? stats_.fromSd : stats_.fetched)++;
		else markFailed(e, true, now);
		break;
	case IconResult::Outcome::Dropped: break;
	case IconResult::Outcome::Transient: markFailed(e, false, now); break;
	case IconResult::Outcome::Permanent: markFailed(e, true, now); break;
	}
	eraseIfIdle(e);
}

bool IconCache::upload(Entry& e, IconResult& r) {
	if (!r.surface || !renderer_) return false;
	const auto t0 = Clock::now();
	SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, r.surface.get());
	if (!tex) {
		LOG("Icon %s: SDL_CreateTextureFromSurface failed: %s", e.slug.c_str(), SDL_GetError());
		return false;
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	const int w = r.surface->w, h = r.surface->h;
	// A refetch after a size change replaces the old picture. shownAt stays, so the new one takes over at full alpha
	// instead of fading in from the placeholder again.
	const bool replaced = e.tex != nullptr;
	if (replaced) dropTexture(e);
	e.tex = tex;
	e.w = w;
	e.h = h;
	e.bytes = size_t(w) * size_t(h) * 4;
	e.texSize = r.size;
	if (!replaced) e.shownAt = -1;
	e.failed = e.permanent = false;
	e.attempts = 0;
	lru_.push_front(&e);
	e.lruPos = lru_.begin();
	textureBytes_ += e.bytes;
	stats_.uploaded++;
	LOG("Icon %s: texture created in %.1f ms (%d textures, %zu KB)", e.slug.c_str(), msSince(t0), (int)lru_.size(), textureBytes_ / 1024);
	evict();
	return true;
}

void IconCache::markFailed(Entry& e, bool permanent, double now) {
	e.failed = true;
	e.attempts++;
	e.permanent = permanent || e.attempts >= cfg_.maxAttempts;
	e.retryAt = now + cfg_.retrySec;
	stats_.failed++;
}

void IconCache::eraseIfIdle(Entry& e) {
	if (e.tex || e.ticket || e.failed) return;
	const std::string key = e.slug;
	map_->erase(key);   // `e` is dead after this
}

void IconCache::touch(Entry& e) {
	if (!e.tex || e.lruPos == lru_.begin()) return;
	lru_.splice(lru_.begin(), lru_, e.lruPos);
}

void IconCache::dropTexture(Entry& e) {
	if (!e.tex) return;
	SDL_DestroyTexture(e.tex);
	e.tex = nullptr;
	lru_.erase(e.lruPos);
	textureBytes_ -= e.bytes;
	e.bytes = 0;
}

void IconCache::evict() {
	while (lru_.size() > 1 && (lru_.size() > cfg_.maxTextures || textureBytes_ > cfg_.maxTextureBytes)) {
		Entry& victim = *lru_.back();
		dropTexture(victim);
		stats_.evictions++;
		eraseIfIdle(victim);
	}
}

bool IconCache::mayRequest(Entry& e, double now) {
	if (e.ticket) return false;
	if (!e.failed) return true;
	if (e.permanent || now < e.retryAt) return false;
	e.failed = false;   // a transient failure is due for another try
	return true;
}

std::optional<IconCache::View> IconCache::get(std::string_view slug, std::string_view url, uint64_t size, double now) {
	if (!cfg_.enabled) return std::nullopt;
	auto it = map_->find(slug);
	if (it == map_->end()) {
		if (!isValidSlug(slug)) return std::nullopt;   // never becomes a file name or a tag
		it = map_->emplace(std::string(slug), Entry{}).first;
		it->second.slug = std::string(slug);
	}
	Entry& e = it->second;
	e.lastReq = frame_;
	const bool stale = e.tex && size != 0 && e.texSize != size;
	if ((!e.tex || stale) && mayRequest(e, now)) enqueue(e, url, size, now);
	if (!e.tex) return std::nullopt;

	touch(e);
	if (e.shownAt < 0) e.shownAt = now;
	if (gen_ > 1 && e.reusedGen != gen_ && !stale) {
		e.reusedGen = gen_;
		LOG("Icon %s: already loaded, reused after the refresh without a download", e.slug.c_str());
	}
	View v;
	v.tex = e.tex;
	v.w = e.w;
	v.h = e.h;
	v.alpha = cfg_.fadeSec <= 0 ? 1.0f : float(std::clamp((now - e.shownAt) / cfg_.fadeSec, 0.0, 1.0));
	return v;
}

void IconCache::enqueue(Entry& e, std::string_view url, uint64_t size, double now) {
	stats_.requested++;
	const bool fetch = cfg_.network;
	std::string full;
	if (fetch) {
		full = cfg_.urlBase.empty() ? std::string(url) : cfg_.urlBase + "/" + e.slug + "/icon.png";
		if (full.empty()) {
			markFailed(e, true, now);   // no icon in the catalog
			return;
		}
	}
	if (!worker_ || !worker_->running()) {
		if (!workerWarned_) LOG("IconCache: the net worker is not running, icons stay placeholders");
		workerWarned_ = true;
		markFailed(e, true, now);
		return;
	}
	IconSpec spec;
	spec.slug = e.slug;
	spec.url = std::move(full);
	spec.cacheDir = cfg_.cacheDir;
	spec.size = size;
	spec.ticket = ++nextTicket_;
	spec.network = fetch;
	spec.totalTimeoutMs = cfg_.fetchTimeoutMs;
	spec.idleTimeoutMs = cfg_.fetchIdleMs;
	spec.maxBytes = cfg_.maxFileBytes;
	spec.ops = cfg_.fileOps;
	const uint64_t ticket = spec.ticket;
	// An empty cacheDir plus offline can never succeed.
	if (!fetch && spec.cacheDir.empty()) {
		markFailed(e, true, now);
		return;
	}
	// With an SD cache the request starts in the IconLocal lane (a short read, served before any download); without one
	// it goes straight to the download.
	const bool viaSd = !spec.cacheDir.empty();
	std::unique_ptr<net::Job> job;
	if (viaSd) job = std::make_unique<IconSdJob>(std::move(spec), shared_);
	else job = std::make_unique<IconNetJob>(std::move(spec), shared_);
	const net::JobId id = worker_->submit(viaSd ? net::JobClass::IconLocal : net::JobClass::Icon, kTagPrefix + e.slug, std::move(job));
	if (!id) {
		markFailed(e, true, now);
		return;
	}
	e.ticket = ticket;
	inFlight_++;
}

void IconCache::dropStale() {
	std::unordered_set<std::string> stale;
	for (auto& [slug, e] : *map_)
		if (e.ticket && frame_ - e.lastReq > cfg_.staleFrames) stale.insert(kTagPrefix + slug);
	if (stale.empty()) return;
	std::vector<std::string> dropped;
	// Runs under the worker's lock: a set lookup and a push_back only. Queued AND running jobs are cancelled: a running
	// download that nobody wants would otherwise hold the single Icon slot until its timeout and the icons of the page
	// on screen would wait behind it. The job notices the flag within one step (30 ms budget, 50 ms socket wait).
	worker_->dropWhere([&](const net::JobInfo& j) {
		if ((j.cls != net::JobClass::Icon && j.cls != net::JobClass::IconLocal) || !stale.count(j.tag)) return false;
		dropped.push_back(j.tag);
		return true;
	});
	for (const std::string& tag : dropped) {
		auto it = map_->find(std::string_view(tag).substr(std::char_traits<char>::length(kTagPrefix)));
		if (it == map_->end()) continue;
		it->second.ticket = 0;   // the job's own Dropped result will find no matching ticket
		if (inFlight_) inFlight_--;
		stats_.dropped++;
		eraseIfIdle(it->second);
	}
	if (!dropped.empty()) LOG("IconCache: cancelled %zu icon requests that are no longer visible", dropped.size());
}

void IconCache::endFrame() {
	if (!cfg_.enabled) return;
	if (inFlight_ && worker_ && (frame_ & 3) == 0) dropStale();
	// A line when something changed, at most about every 5 s at 60 fps.
	if (frame_ - lastLogFrame_ >= 300) {
		Stats s = stats();
		if (s.textures != lastLogged_.textures || s.evictions != lastLogged_.evictions || s.requested != lastLogged_.requested ||
		    s.dropped != lastLogged_.dropped || s.failed != lastLogged_.failed) {
			LOG("IconCache: %zu textures (%zu KB, cap %zu), %llu evictions, %llu requested, %llu uploaded, %llu failed, %llu dropped, %llu stale results, %zu in flight",
			    s.textures, s.textureBytes / 1024, cfg_.maxTextures, (unsigned long long)s.evictions, (unsigned long long)s.requested,
			    (unsigned long long)s.uploaded, (unsigned long long)s.failed, (unsigned long long)s.dropped, (unsigned long long)s.stale, s.inFlight);
			lastLogged_ = s;
		}
		lastLogFrame_ = frame_;
	}
}

void IconCache::startPrune(const std::set<std::string>& keep) {
	if (!cfg_.enabled || pruned_ || cfg_.cacheDir.empty() || keep.empty() || !worker_ || !worker_->running()) return;
	pruned_ = true;
	if (!worker_->submit(net::JobClass::Background, "iconprune", std::make_unique<IconPruneJob>(cfg_.cacheDir, keep)))
		LOG("IconCache: the prune job was rejected");
}
