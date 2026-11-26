#include "catalog/CatalogCache.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <jansson.h>

#include "catalog/OscApi.h"
#include "core/Log.h"
#include "core/Time.h"

namespace catcache {

// ---------------------------------------------------------------------------------------------------------------------
bool usable(const std::optional<Meta>& meta, std::string_view baseUrl) {
	return meta && meta->format == kFormat && meta->baseUrl == baseUrl;
}

bool fresh(const std::optional<Meta>& meta, int64_t now, const Policy& p) {
	if (!usable(meta, p.baseUrl) || meta->appVersion != p.appVersion) return false;
	if (now <= 0 || meta->fetchedAt <= 0 || p.ttlSec <= 0) return false;
	if (meta->fetchedAt > now + kClockSkewSec) return false; // from the future: the clock was wrong then or is now
	const int64_t age = now > meta->fetchedAt ? now - meta->fetchedAt : 0;
	return age < p.ttlSec;
}

Source decideSource(const std::optional<Meta>& meta, int64_t now, const Policy& p) {
	const bool have = usable(meta, p.baseUrl);
	// Cache only: the TTL is irrelevant, and so is the refresh rule (a refresh re-reads the cache).
	if (p.offline) return have ? Source::UseCache : Source::Fail;
	// Refresh: the in-memory catalog is at least as new as anything on SD, so a failed fetch must not fall back to it.
	if (p.refreshMode) return Source::Fetch;
	if (!have) return Source::Fetch;
	if (p.ignoreTtl) return Source::FetchWithFallback;
	return fresh(meta, now, p) ? Source::UseCache : Source::FetchWithFallback;
}

const char* sourceName(Source s) {
	switch (s) {
	case Source::UseCache: return "UseCache";
	case Source::Fetch: return "Fetch";
	case Source::FetchWithFallback: return "FetchWithFallback";
	case Source::Fail: return "Fail";
	}
	return "?";
}

// ---------------------------------------------------------------------------------------------------------------------
uint64_t fnv1a64(std::string_view data) {
	uint64_t h = 14695981039346656037ULL;
	for (unsigned char c : data) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

namespace {
constexpr const char* kManifest = "cache.json";
// The body base names; the files are "<base>.<generation>.json".
constexpr const char* kInfoBase = "information";
constexpr const char* kContentsBase = "contents";
constexpr const char* kFeaturedBase = "featured";
constexpr const char* kBases[] = {kInfoBase, kContentsBase, kFeaturedBase};

std::string hex64(uint64_t v) {
	char buf[17];
	std::snprintf(buf, sizeof buf, "%016" PRIx64, v);
	return buf;
}

bool parseHex64(const char* s, uint64_t& out) {
	if (!s || std::strlen(s) != 16) return false;
	uint64_t v = 0;
	for (const char* p = s; *p; p++) {
		int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
		if (d < 0) return false;
		v = v << 4 | (uint64_t)d;
	}
	out = v;
	return true;
}

bool getInt(json_t* o, const char* key, int64_t lo, int64_t hi, int64_t& out) {
	json_t* v = json_object_get(o, key);
	if (!json_is_integer(v)) return false;
	json_int_t n = json_integer_value(v);
	if (n < lo || n > hi) return false;
	out = n;
	return true;
}

bool getString(json_t* o, const char* key, size_t maxLen, std::string& out) {
	json_t* v = json_object_get(o, key);
	if (!json_is_string(v) || json_string_length(v) > maxLen) return false;
	out.assign(json_string_value(v), json_string_length(v));
	return true;
}

// One entry of "files": {"size": N, "fnv": "<16 hex>"}, N within the endpoint's byte limit.
bool parseFile(json_t* files, const char* key, size_t limit, FileInfo& out) {
	json_t* e = json_object_get(files, key);
	if (!e) return true; // absent
	if (!json_is_object(e)) return false;
	int64_t size = 0;
	if (!getInt(e, "size", 1, (int64_t)limit, size)) return false;
	json_t* h = json_object_get(e, "fnv");
	if (!json_is_string(h) || !parseHex64(json_string_value(h), out.hash)) return false;
	out.present = true;
	out.size = (uint64_t)size;
	return true;
}

bool parseManifest(const std::string& text, Meta& m) {
	json_error_t err;
	json_t* root = json_loadb(text.data(), text.size(), JSON_REJECT_DUPLICATES, &err);
	if (!root) return false;
	bool ok = false;
	do {
		if (!json_is_object(root)) break;
		int64_t fmt = 0, at = 0, gen = 0;
		if (!getInt(root, "format", 0, 1000000, fmt) || !getInt(root, "fetchedAt", 0, 253402300799LL, at) ||
		    !getInt(root, "generation", 0, 0xFFFFFFFFLL, gen))
			break;
		Meta t;
		t.format = (int)fmt;
		t.fetchedAt = at;
		t.generation = (uint32_t)gen;
		if (!getString(root, "app", 64, t.appVersion) || !getString(root, "base", 512, t.baseUrl)) break;
		json_t* files = json_object_get(root, "files");
		if (!json_is_object(files)) break;
		if (!parseFile(files, "information", oscapi::kMaxInformationBytes, t.information) ||
		    !parseFile(files, "contents", oscapi::kMaxContentsBytes, t.contents) ||
		    !parseFile(files, "featured", oscapi::kMaxFeaturedBytes, t.featured))
			break;
		if (!t.contents.present) break; // nothing to show without /contents
		m = std::move(t);
		ok = true;
	} while (false);
	json_decref(root);
	return ok;
}

std::string buildManifest(const Meta& m) {
	json_t* root = json_object();
	json_object_set_new(root, "format", json_integer(m.format));
	json_object_set_new(root, "app", json_string(m.appVersion.c_str()));
	json_object_set_new(root, "base", json_string(m.baseUrl.c_str()));
	json_object_set_new(root, "fetchedAt", json_integer(m.fetchedAt));
	json_object_set_new(root, "generation", json_integer(m.generation));
	json_t* files = json_object();
	auto add = [&](const char* key, const FileInfo& f) {
		if (!f.present) return;
		json_t* e = json_object();
		json_object_set_new(e, "size", json_integer((json_int_t)f.size));
		json_object_set_new(e, "fnv", json_string(hex64(f.hash).c_str()));
		json_object_set_new(files, key, e);
	};
	add("information", m.information);
	add("contents", m.contents);
	add("featured", m.featured);
	json_object_set_new(root, "files", files);
	char* dumped = json_dumps(root, JSON_INDENT(1) | JSON_SORT_KEYS);
	std::string out = dumped ? dumped : "";
	std::free(dumped);
	json_decref(root);
	if (!out.empty()) out += '\n';
	return out;
}

int plainRemove(const char* p) { return std::remove(p); }

using timeutil::msSince;

// What kind of file of ours `name` is, if any: the manifest itself, a body "<base>.<G>.json" (G in `gen`), or a leftover
// (a .tmp, the manifest's .bak, a body of the pre-generation layout such as "contents.json"). Anything else is not ours
// and is never touched.
enum class Kind { Foreign, Manifest, Body, Leftover };

Kind classify(const std::string& name, uint32_t& gen) {
	gen = 0;
	if (name == kManifest) return Kind::Manifest;
	if (name == std::string(kManifest) + ".bak" || name == std::string(kManifest) + ".tmp") return Kind::Leftover;
	for (const char* base : kBases) {
		const size_t n = std::strlen(base);
		if (name.compare(0, n, base) != 0 || name.size() <= n || name[n] != '.') continue;
		const std::string rest = name.substr(n + 1);
		if (rest == "json" || rest == "json.bak" || rest == "json.tmp") return Kind::Leftover; // format 1
		size_t i = 0;
		uint64_t v = 0;
		while (i < rest.size() && i < 10 && rest[i] >= '0' && rest[i] <= '9') v = v * 10 + (uint64_t)(rest[i++] - '0');
		if (i == 0 || v > 0xFFFFFFFFULL) continue;
		const std::string tail = rest.substr(i);
		if (tail == ".json") { gen = (uint32_t)v; return Kind::Body; }
		if (tail == ".json.tmp") { gen = (uint32_t)v; return Kind::Leftover; }
	}
	return Kind::Foreign;
}
} // namespace

std::string Cache::bodyName(const char* base, uint32_t generation) {
	return std::string(base) + "." + std::to_string(generation) + ".json";
}

ReadStatus Cache::readRaw(const std::string& file, std::string& out, size_t maxBytes) const {
	return ops_.read ? ops_.read(file.c_str(), out, maxBytes) : readFileCapped(file, out, maxBytes);
}

void Cache::removeFile(const std::string& file) const {
	(ops_.remove ? ops_.remove : plainRemove)(file.c_str());
}

Cache::Status Cache::readMeta(Meta& out) const {
	bool damaged = false;
	const char* why = "missing";
	for (const char* suffix : {"", ".bak"}) {
		std::string text;
		const ReadStatus rs = readRaw(path(kManifest, suffix), text, kMaxManifestBytes);
		if (rs == ReadStatus::Missing) continue;
		if (rs == ReadStatus::IoError) {
			// No fallback to the .bak after an I/O error on cache.json: the .bak can be a stale leftover whose bodies
			// are gone, and trusting it would turn a hiccup into "corrupt, delete everything".
			return Status::Unreadable;
		}
		Meta m;
		// TooLarge (a manifest is a few hundred bytes) and a parse failure are both data that cannot be a manifest.
		if (rs == ReadStatus::Ok && parseManifest(text, m)) {
			out = std::move(m);
			if (suffix[0]) LOG("Catalog cache: %s is %s, using %s.bak (an interrupted write)", kManifest, why, kManifest);
			return Status::Ok;
		}
		damaged = true;
		if (!suffix[0]) why = "damaged";
	}
	return damaged ? Status::Corrupt : Status::Missing;
}

Cache::Status Cache::readBodies(const Meta& meta, Bodies& out) const {
	out = Bodies{};
	struct Item {
		const char* base;
		const FileInfo* info;
		std::optional<std::string>* dest;
		size_t limit;
	};
	const Item items[] = {
		{kInfoBase, &meta.information, &out.information, oscapi::kMaxInformationBytes},
		{kContentsBase, &meta.contents, &out.contents, oscapi::kMaxContentsBytes},
		{kFeaturedBase, &meta.featured, &out.featured, oscapi::kMaxFeaturedBytes},
	};
	for (const Item& it : items) {
		if (!it.info->present) continue;
		if (it.info->size > it.limit) return Status::Corrupt;
		const std::string name = bodyName(it.base, meta.generation);
		std::string body;
		const ReadStatus rs = readRaw(path(name), body, it.limit);
		Status bad = Status::Ok;
		if (rs == ReadStatus::IoError) {
			bad = Status::Unreadable; // the card hiccuped: not a verdict on the data
			LOG("Catalog cache: %s could not be read (I/O error), keeping the cache", name.c_str());
		} else if (rs == ReadStatus::Missing) {
			bad = Status::Corrupt; // the manifest lists it, so it must exist
			LOG("Catalog cache: %s is missing", name.c_str());
		} else if (rs == ReadStatus::TooLarge) {
			// Larger than the endpoint's byte limit, so it can never be a valid body: the same as a failed hash.
			bad = Status::Corrupt;
			LOG("Catalog cache: %s is larger than the limit", name.c_str());
		} else if (body.size() != it.info->size || fnv1a64(body) != it.info->hash) {
			bad = Status::Corrupt;
			LOG("Catalog cache: %s does not match the manifest", name.c_str());
		}
		if (bad != Status::Ok) {
			out = Bodies{};
			return bad;
		}
		*it.dest = std::move(body);
	}
	return Status::Ok;
}

Cache::Writer::Writer(const Cache& cache, Bodies&& bodies, std::string_view baseUrl, std::string_view appVersion,
                      int64_t fetchedAt)
    : cache_(cache), bodies_(std::move(bodies)), baseUrl_(baseUrl), appVersion_(appVersion), fetchedAt_(fetchedAt) {}

// Bodies in write order, with where their size and hash go in the manifest.
namespace {
struct BodyRef {
	const char* base;
	std::optional<std::string> Bodies::*body;
	FileInfo Meta::*info;
};
const BodyRef kBodyRefs[] = {
	{kInfoBase, &Bodies::information, &Meta::information},
	{kContentsBase, &Bodies::contents, &Meta::contents},
	{kFeaturedBase, &Bodies::featured, &Meta::featured},
};
} // namespace

bool Cache::Writer::step() {
	if (stage_ == Stage::Finished) return true;
	const auto t0 = std::chrono::steady_clock::now();
	switch (stage_) {
	case Stage::Prepare:
		if (!prepare()) fail();
		else stage_ = Stage::Bodies;
		break;
	case Stage::Bodies:
		if (!writeNextBody()) fail();
		break;
	case Stage::Commit:
		if (!commit()) fail();
		else stage_ = Stage::Cleanup;
		break;
	case Stage::Cleanup:
		// Only now are the older generations expendable. A failure here just leaves harmless files behind, which the next
		// successful write removes.
		cache_.cleanup(&gen_);
		ok_ = true;
		stage_ = Stage::Finished;
		break;
	case Stage::Finished: break;
	}
	busyMs_ += msSince(t0);
	if (stage_ == Stage::Finished && ok_)
		LOG("Catalog cache: wrote %zu KB in %.0f ms of work (generation %u)", (total_ + 512) / 1024, busyMs_, (unsigned)gen_);
	return stage_ == Stage::Finished;
}

bool Cache::Writer::prepare() {
	if (!bodies_.contents || bodies_.contents->empty()) return false;
	if (!makeDirs(cache_.dir_)) {
		LOG("Catalog cache: cannot create %s, running uncached", cache_.dir_.c_str());
		return false;
	}
	// The new generation is past everything that is there, so no name it writes exists yet (see the header).
	uint32_t highest = 0;
	std::vector<std::string> names;
	listDir(cache_.dir_, names);
	for (const std::string& n : names) {
		uint32_t g;
		if (classify(n, g) != Kind::Foreign && g > highest) highest = g;
	}
	for (const char* suffix : {"", ".bak"}) {
		std::string text;
		Meta old;
		if (cache_.readRaw(cache_.path(kManifest, suffix), text, kMaxManifestBytes) == ReadStatus::Ok && parseManifest(text, old) &&
		    old.generation > highest)
			highest = old.generation;
	}
	if (highest == 0xFFFFFFFFu) {
		LOG("Catalog cache: the generation counter is exhausted, running uncached");
		return false;
	}
	gen_ = highest + 1;
	meta_.format = kFormat;
	meta_.appVersion = appVersion_;
	meta_.baseUrl = baseUrl_;
	meta_.fetchedAt = fetchedAt_ > 0 ? fetchedAt_ : 0;
	meta_.generation = gen_;
	return true;
}

bool Cache::Writer::writeNextBody() {
	constexpr int kCount = sizeof(kBodyRefs) / sizeof(kBodyRefs[0]);
	// An empty optional body is as good as absent (the manifest cannot describe a size-0 file); skip to the next real one.
	while (nextBody_ < kCount) {
		const BodyRef& r = kBodyRefs[nextBody_++];
		const std::optional<std::string>& body = bodies_.*r.body;
		if (!body || body->empty()) continue;
		if (!writeFileAtomic(cache_.path(bodyName(r.base, gen_)), *body, cache_.ops_)) {
			LOG("Catalog cache: writing %s failed, running uncached", bodyName(r.base, gen_).c_str());
			return false;
		}
		FileInfo& fi = meta_.*r.info;
		fi.present = true;
		fi.size = body->size();
		fi.hash = fnv1a64(*body);
		total_ += body->size();
		break;
	}
	if (nextBody_ >= kCount) stage_ = Stage::Commit;
	return true;
}

bool Cache::Writer::commit() {
	// The commit point. libfat cannot rename over cache.json, so commitFile parks the old one as cache.json.bak first;
	// the reader knows that fallback. A manifest that is not in place leaves the old generation fully readable.
	const std::string manifest = buildManifest(meta_);
	if (manifest.empty() || !writeFileAtomic(cache_.path(kManifest), manifest, cache_.ops_)) {
		LOG("Catalog cache: writing the manifest failed, running uncached");
		return false;
	}
	committed_ = true;
	return true;
}

// A failed attempt removes what it wrote (on a nearly full card the leftovers would make the next attempt worse). Only
// files of generation `gen_`, which no manifest names yet.
void Cache::Writer::fail() {
	if (gen_ != 0) {
		for (const BodyRef& r : kBodyRefs) {
			cache_.removeFile(cache_.path(bodyName(r.base, gen_)));
			cache_.removeFile(cache_.path(bodyName(r.base, gen_), ".tmp"));
		}
		cache_.removeFile(cache_.path(kManifest, ".tmp"));
	}
	ok_ = false;
	stage_ = Stage::Finished;
}

void Cache::Writer::abandon() {
	if (stage_ == Stage::Finished) return;
	if (committed_) {
		// The new cache is in place; only the tidying of old generations is skipped.
		ok_ = true;
		stage_ = Stage::Finished;
		return;
	}
	fail();
}

bool Cache::write(const Bodies& bodies, std::string_view baseUrl, std::string_view appVersion, int64_t fetchedAt,
                  uint32_t* generationOut) {
	Writer w(*this, Bodies(bodies), baseUrl, appVersion, fetchedAt);
	while (!w.step()) {}
	if (w.ok() && generationOut) *generationOut = w.generation();
	return w.ok();
}

void Cache::cleanup(const uint32_t* keepGeneration) const {
	std::vector<std::string> names;
	if (!listDir(dir_, names)) return;
	std::vector<std::string> doomed, manifestFirst;
	for (const std::string& n : names) {
		uint32_t g;
		const Kind k = classify(n, g);
		if (k == Kind::Foreign) continue;
		if (keepGeneration) {
			if (k == Kind::Manifest) continue;
			if (k == Kind::Body) {
				bool current = false;
				for (const char* base : kBases) current = current || n == bodyName(base, *keepGeneration);
				if (current) continue;
			}
		}
		(n.compare(0, std::strlen(kManifest), kManifest) == 0 ? manifestFirst : doomed).push_back(n);
	}
	// The manifest's own files first: a cut halfway then never leaves a manifest whose bodies are gone.
	for (const std::string& n : manifestFirst) removeFile(path(n));
	for (const std::string& n : doomed) removeFile(path(n));
}

void Cache::invalidate() const {
	// The manifest first (even when the directory cannot be listed), then everything else of ours.
	for (const char* suffix : {"", ".bak", ".tmp"}) removeFile(path(kManifest, suffix));
	cleanup(nullptr);
}

} // namespace catcache
