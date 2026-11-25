#pragma once
// The on-SD catalog cache (Increment 10): the three raw API bodies plus a manifest, under <dataDir>/cache/.
// No SDL and no network: it only touches the filesystem, so the host tests drive it directly.
//
// LAYOUT (the cache directory)
//   cache.json                the manifest, the single commit point. Format version, writing app version, base URL,
//                             fetch time, generation G, and for each body its size and FNV-1a 64 hash.
//   information.<G>.json      raw /information body (optional: the API may have been unusable when it was fetched)
//   contents.<G>.json         raw /contents body (required)
//   featured.<G>.json         raw /featured-app body (optional)
//   *.tmp, cache.json.bak     the leftovers of an interrupted write
// The body names are COMPUTED from G and the constant base names (bodyName()); a name is never read from the manifest.
//
// THE INVARIANT (what the write protocol guarantees, and the tests sweep)
//   After ANY sequence of interrupted or failed writes (a crash or a failing call at any filesystem call, any number of
//   times in a row), a reader sees the last fully committed cache or a newer fully committed one: never a mix of two
//   fetches, and never nothing while a committed cache existed.
// Why it holds:
//   1. A write picks G' = (highest generation found anywhere in the directory) + 1, so the names it writes did not
//      exist before. It never overwrites or deletes a file of any older generation while it works.
//   2. A body file is complete before its name appears (tmp file, then rename), and is never modified afterwards.
//   3. Only the manifest says which generation is current. It is replaced last, by one rename, or on libfat (no rename
//      over an existing file) by parking the old manifest as cache.json.bak, then renaming the new one in. A cut
//      between the two renames leaves cache.json missing, and the reader then takes cache.json.bak, which is the
//      previous committed manifest, whose bodies are all still there (step 1).
//   4. Old generations are deleted only after the manifest commit succeeded, so what a reader can reach is always one
//      manifest plus the complete body set of exactly its generation. A failed or interrupted write leaves only files
//      of a generation no manifest names; the next write numbers past them, and the next success deletes them.
//   Garbage is bounded: a successful write deletes every generation but its own, every .tmp and the manifest .bak (only
//   names matching our patterns, never anything else in the directory).
//
// THREADING: the cache does blocking SD I/O (a 455 KB read or write takes a noticeable time on a Wii), so it is only
// ever used from the network worker's Catalog job, never from draw(), which keeps the frame rate up. The worker runs
// one Catalog job at a time, so two writes never overlap.
//
// UNTRUSTED INPUT: the SD card can be edited or corrupted. Every file is read with a size cap, checked against the
// manifest's size and hash, and the bodies then go through the same OscApi limits as a network response. FNV-1a 64 is
// there to catch corruption (a torn write, a bad sector), NOT deliberate tampering: someone who can edit the SD card can
// edit the manifest's hash as well. A tampered body is still bounded by buildCatalog's limits, which is the same trust
// level as a body from the network. Anything that does not add up is "Corrupt": the caller deletes the cache and
// fetches. A read that failed with an I/O error is "Unreadable", which is NOT a verdict on the data: nothing is deleted.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/FileUtil.h"

namespace catcache {

// Bump when the manifest or the meaning of the files changes: caches with another number are ignored (and replaced by
// the next fetch).
inline constexpr int kFormat = 2; // 2: generation-named bodies (1 was contents.json without a generation)
// Catalog freshness: a cache younger than this is used with no network at all (kept in one place; the dev option
// cachettl= overrides it).
inline constexpr int kTtlSec = 3600;
// A cache stamped further in the future than this (a Wii whose clock was reset) is never "fresh".
inline constexpr int kClockSkewSec = 300;
inline constexpr size_t kMaxManifestBytes = 8 * 1024;

struct FileInfo {
	bool present = false;
	uint64_t size = 0;
	uint64_t hash = 0;
};

struct Meta {
	int format = 0;
	std::string appVersion;
	std::string baseUrl;
	int64_t fetchedAt = 0;  // unix seconds
	uint32_t generation = 0; // +1 per write; lets a log or a test tell two caches apart
	FileInfo information, contents, featured;
};

struct Bodies {
	std::optional<std::string> information, contents, featured;
};

// ---------------------------------------------------------------------------------------------------------------------
// The decision (pure, so a table of cases covers it).
enum class Source {
	UseCache,          // read the cache, no network
	Fetch,             // fetch; a failure is an error (no stale fallback)
	FetchWithFallback, // fetch; on failure use the (stale) cache and flag the catalog offline
	Fail,              // nothing to show and not allowed to fetch
};

struct Policy {
	std::string_view appVersion;
	std::string_view baseUrl;
	int ttlSec = kTtlSec;
	bool offline = false;    // cache only, never the network
	bool ignoreTtl = false;  // `--refresh` at startup: fetch even when fresh, but a failure may still use the cache
	bool refreshMode = false; // the user pressed Refresh: always fetch, never a stale fallback
};

// The cache is usable at all: current format and written for this server. (A cache from another app version is
// still usable, as a fallback: the raw bodies are re-parsed by the current parser.)
bool usable(const std::optional<Meta>& meta, std::string_view baseUrl);
// Usable, written by this app version, and younger than the TTL (a stamp from the future, beyond the skew allowance,
// is never fresh; neither is an unknown clock, now <= 0).
bool fresh(const std::optional<Meta>& meta, int64_t now, const Policy& p);
Source decideSource(const std::optional<Meta>& meta, int64_t now, const Policy& p);
const char* sourceName(Source s);

// ---------------------------------------------------------------------------------------------------------------------
uint64_t fnv1a64(std::string_view data);

class Cache {
public:
	// Ok; Missing (nothing there); Corrupt (data was read and did not validate, or is too large to be valid: delete it);
	// Unreadable (an I/O error: "missing this time", do not delete, the data may be fine).
	enum class Status { Ok, Missing, Corrupt, Unreadable };

	explicit Cache(std::string dir, FileOps ops = {}) : dir_(std::move(dir)), ops_(ops) {}

	// Reads and validates the manifest only (a few hundred bytes). Falls back to "cache.json.bak" when cache.json is
	// missing (a power cut between the two renames of its replacement) or damaged, but not after an I/O error (Unreadable).
	Status readMeta(Meta& out) const;
	// Reads the bodies of the manifest's generation and checks size and hash. Corrupt unless every listed body matches.
	Status readBodies(const Meta& meta, Bodies& out) const;

	// Writes a new cache as the next generation (see the invariant above). Empty optional bodies count as absent;
	// `contents` must be set and not empty. Returns false (logged) if any step failed: the previous cache stays
	// readable. A cleanup that fails afterwards is not an error. `generationOut`, when given, gets the new generation.
	bool write(const Bodies& bodies, std::string_view baseUrl, std::string_view appVersion, int64_t fetchedAt,
	           uint32_t* generationOut = nullptr);

	// Removes the manifest first (so a partial delete leaves "missing", never "valid with holes"), then every body,
	// backup and tmp file of ours, whatever its generation.
	void invalidate() const;

	// "<base>.<generation>.json", e.g. "contents.7.json".
	static std::string bodyName(const char* base, uint32_t generation);

private:
	std::string path(const std::string& name, const char* suffix = "") const { return dir_ + "/" + name + suffix; }
	ReadStatus readRaw(const std::string& file, std::string& out, size_t maxBytes) const;
	void removeFile(const std::string& file) const;
	// Deletes everything of ours in the directory except the manifest and the bodies of `keepGeneration`.
	void cleanup(const uint32_t* keepGeneration) const;
	std::string dir_;
	FileOps ops_;
};

} // namespace catcache
