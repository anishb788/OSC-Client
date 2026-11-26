// Host tests for the catalog cache (Increment 10): the freshness decision table, the manifest and the bodies it
// guards, corruption handling, and crash safety of the multi-file write. No network, no SDL. The sweeps cut writes at
// every filesystem call (through the FileOps injection FileSink also uses), also several writes in a row, and check the
// invariant of CatalogCache.h: a reader always sees a fully committed cache, never a mix and never nothing.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catalog/CatalogCache.h"
#include "catalog/OscApi.h"

using namespace catcache;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::string slurp(const std::string& p) {
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}
static void spit(const std::string& p, const std::string& s) {
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f << s;
}

struct TempDir {
	std::string path;
	TempDir() {
		char tmpl[] = "/tmp/openshop-cache-XXXXXX";
		path = mkdtemp(tmpl);
	}
	~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
	std::string sub(const std::string& s) const { return path + "/" + s; }
};

static const std::string kBase = "http://example.test/api/v4";
static const std::string kApp = "0.2.0";

static Bodies makeBodies(const std::string& tag, bool info = true, bool featured = true) {
	Bodies b;
	if (info) b.information = "{\"info\":\"" + tag + "\"}";
	b.contents = "[{\"contents\":\"" + tag + std::string(300, 'x') + "\"}]";
	if (featured) b.featured = "{\"featured\":\"" + tag + "\"}";
	return b;
}

static bool sameBodies(const Bodies& a, const Bodies& b) {
	return a.information == b.information && a.contents == b.contents && a.featured == b.featured;
}

// ---------------------------------------------------------------------------------------------------------------------
static Meta metaAt(int64_t t, const std::string& app = kApp, const std::string& base = kBase, int format = kFormat) {
	Meta m;
	m.format = format;
	m.appVersion = app;
	m.baseUrl = base;
	m.fetchedAt = t;
	m.contents.present = true;
	return m;
}

static Source decide(const std::optional<Meta>& m, int64_t now, bool offline, bool ignoreTtl, bool refresh, int ttl = kTtlSec,
                     const std::string& app = kApp) {
	Policy p;
	p.appVersion = app;
	p.baseUrl = kBase;
	p.ttlSec = ttl;
	p.offline = offline;
	p.ignoreTtl = ignoreTtl;
	p.refreshMode = refresh;
	return decideSource(m, now, p);
}

static void testDecisionTable() {
	const int64_t now = 1'800'000'000;
	const std::optional<Meta> none;
	const std::optional<Meta> fresh10m = metaAt(now - 600);
	const std::optional<Meta> stale2h = metaAt(now - 7200);
	// Startup.
	CHECK(decide(none, now, false, false, false) == Source::Fetch);
	CHECK(decide(fresh10m, now, false, false, false) == Source::UseCache);
	CHECK(decide(stale2h, now, false, false, false) == Source::FetchWithFallback);
	CHECK(decide(metaAt(now - (kTtlSec - 1)), now, false, false, false) == Source::UseCache);   // just inside
	CHECK(decide(metaAt(now - kTtlSec), now, false, false, false) == Source::FetchWithFallback); // exactly the TTL: stale
	CHECK(decide(fresh10m, now, false, false, false, /*ttl=*/0) == Source::FetchWithFallback);  // cachettl=0
	CHECK(decide(stale2h, now, false, false, false, /*ttl=*/10000) == Source::UseCache);
	// Clock skew: a stamp from the future is never fresh (small skew is tolerated), an unknown clock neither.
	CHECK(decide(metaAt(now + 10), now, false, false, false) == Source::UseCache);
	CHECK(decide(metaAt(now + kClockSkewSec + 1), now, false, false, false) == Source::FetchWithFallback);
	CHECK(decide(metaAt(now + 10 * 365 * 86400LL), now, false, false, false) == Source::FetchWithFallback);
	CHECK(decide(fresh10m, 0, false, false, false) == Source::FetchWithFallback);
	CHECK(decide(metaAt(0), now, false, false, false) == Source::FetchWithFallback);
	// Another app version: not fresh, but still a usable fallback (the raw bodies are re-parsed).
	CHECK(decide(metaAt(now - 60, "0.1.0"), now, false, false, false) == Source::FetchWithFallback);
	// Another format version or another server: as good as missing.
	CHECK(decide(metaAt(now - 60, kApp, kBase, kFormat + 1), now, false, false, false) == Source::Fetch);
	CHECK(decide(metaAt(now - 60, kApp, kBase, 0), now, false, false, false) == Source::Fetch);
	CHECK(decide(metaAt(now - 60, kApp, "http://other/api"), now, false, false, false) == Source::Fetch);
	// --refresh at startup: fetch even when fresh, but the stale cache may still be the fallback.
	CHECK(decide(fresh10m, now, false, true, false) == Source::FetchWithFallback);
	CHECK(decide(none, now, false, true, false) == Source::Fetch);
	// --offline: cache only, whatever its age; an error without a usable one.
	CHECK(decide(fresh10m, now, true, false, false) == Source::UseCache);
	CHECK(decide(stale2h, now, true, false, false) == Source::UseCache);
	CHECK(decide(metaAt(now + 99999), now, true, false, false) == Source::UseCache);
	CHECK(decide(none, now, true, false, false) == Source::Fail);
	CHECK(decide(metaAt(now, kApp, kBase, kFormat + 1), now, true, false, false) == Source::Fail);
	CHECK(decide(stale2h, now, true, true, false) == Source::UseCache);
	// Refresh mode: always fetch, TTL ignored, and never a stale fallback.
	CHECK(decide(fresh10m, now, false, false, true) == Source::Fetch);
	CHECK(decide(stale2h, now, false, false, true) == Source::Fetch);
	CHECK(decide(none, now, false, false, true) == Source::Fetch);
	CHECK(decide(fresh10m, now, false, true, true) == Source::Fetch);
	// Refresh with --offline re-reads the cache; without one it fails.
	CHECK(decide(fresh10m, now, true, false, true) == Source::UseCache);
	CHECK(decide(stale2h, now, true, false, true) == Source::UseCache);
	CHECK(decide(none, now, true, false, true) == Source::Fail);
	// The constant the spec names.
	CHECK(kTtlSec == 3600);
}

static void testHash() {
	CHECK(fnv1a64("") == 0xcbf29ce484222325ULL);
	CHECK(fnv1a64("a") == 0xaf63dc4c8601ec8cULL); // the published FNV-1a 64 test vector
	CHECK(fnv1a64("abc") != fnv1a64("abd"));
}

// ---------------------------------------------------------------------------------------------------------------------
static std::vector<std::string> listing(const std::string& dir) {
	std::vector<std::string> v;
	std::error_code ec;
	for (auto& e : fs::directory_iterator(dir, ec)) v.push_back(e.path().filename().string());
	std::sort(v.begin(), v.end());
	return v;
}

// The directory holds exactly the manifest and the bodies of one generation (what a successful write leaves).
static bool onlyCurrent(const std::string& dir, uint32_t gen, bool info, bool featured) {
	std::vector<std::string> want = {"cache.json", Cache::bodyName("contents", gen)};
	if (info) want.push_back(Cache::bodyName("information", gen));
	if (featured) want.push_back(Cache::bodyName("featured", gen));
	std::sort(want.begin(), want.end());
	return listing(dir) == want;
}

static void testRoundTrip() {
	TempDir t;
	Cache c(t.sub("cache"));   // the directory does not exist yet: write() creates it
	Meta m;
	CHECK(c.readMeta(m) == Cache::Status::Missing);
	Bodies b = makeBodies("one");
	uint32_t gen = 0;
	CHECK(c.write(b, kBase, kApp, 1234567, &gen) && gen == 1);
	CHECK(c.readMeta(m) == Cache::Status::Ok);
	CHECK(m.format == kFormat && m.appVersion == kApp && m.baseUrl == kBase && m.fetchedAt == 1234567 && m.generation == 1);
	CHECK(m.information.present && m.contents.present && m.featured.present);
	CHECK(m.contents.size == b.contents->size());
	Bodies r;
	CHECK(c.readBodies(m, r) == Cache::Status::Ok);
	CHECK(sameBodies(b, r));
	// The manifest and the three generation-named bodies exist; no tmp or bak is left over.
	CHECK(Cache::bodyName("contents", 7) == "contents.7.json");
	CHECK(onlyCurrent(t.sub("cache"), 1, true, true));
	// Overwriting works (the new generation has new names: nothing is replaced, so the libfat case never arises for
	// bodies) and leaves nothing behind either.
	Bodies b2 = makeBodies("two", /*info=*/false, /*featured=*/false);
	CHECK(c.write(b2, kBase, kApp, 99, &gen) && gen == 2);
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.generation == 2 && !m.information.present && !m.featured.present);
	CHECK(c.readBodies(m, r) == Cache::Status::Ok && sameBodies(b2, r));
	CHECK(onlyCurrent(t.sub("cache"), 2, false, false));   // generation 1 and its optional bodies are gone
	// contents is required (absent or empty).
	Bodies none;
	CHECK(!c.write(none, kBase, kApp, 1));
	Bodies emptyContents = b;
	emptyContents.contents = "";
	CHECK(!c.write(emptyContents, kBase, kApp, 1));
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.generation == 2);   // the failed attempts changed nothing
	// A base URL with characters that need JSON escaping survives.
	CHECK(c.write(b, "http://h/\"q\"\\x", kApp, 5, &gen) && gen == 3);
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.baseUrl == "http://h/\"q\"\\x");
	c.invalidate();
	CHECK(c.readMeta(m) == Cache::Status::Missing);
	CHECK(listing(t.sub("cache")).empty());
	// After an invalidate the numbering starts over (nothing of any generation is left to collide with).
	CHECK(c.write(b, kBase, kApp, 5, &gen) && gen == 1);
	// A cache directory that cannot be created (a file is in the way): write fails cleanly, nothing is thrown.
	spit(t.sub("blocker"), "x");
	Cache bad(t.sub("blocker/cache"));
	CHECK(!bad.write(b, kBase, kApp, 1));
}

// M3 1a: the cache stores the raw API bodies (https archive URLs), so re-parsing them gives the rebuilt http URLs.
static void testCachedCatalogRebuildsUrls() {
	std::string contents = slurp("tests/fixtures/contents.json");
	CHECK(!contents.empty() && contents.find("https://hbb1.oscwii.org/api/contents/") != std::string::npos);
	TempDir t;
	Cache c(t.sub("cache"));
	Bodies b;
	b.contents = contents;
	CHECK(c.write(b, kBase, kApp, 1000));
	Meta m;
	Bodies r;
	CHECK(c.readMeta(m) == Cache::Status::Ok && c.readBodies(m, r) == Cache::Status::Ok && r.contents);
	oscapi::BuildResult br = oscapi::buildCatalog(std::nullopt, r.contents ? *r.contents : std::string(), std::nullopt);
	CHECK(br.ok() && br.catalog.apps.size() > 300);
	for (const auto& a : br.catalog.apps) {
		CHECK(a.archiveUrl == "http://hbb1.oscwii.org/api/contents/" + a.slug + "/" + a.slug + ".zip" && a.installable());
	}
}

// M1: an empty optional body is treated as absent (a size-0 entry would make the manifest unparseable).
static void testEmptyOptionalBodies() {
	TempDir t;
	Cache c(t.sub("c"));
	Bodies b = makeBodies("e");
	b.information = "";
	b.featured = "";
	CHECK(c.write(b, kBase, kApp, 10));
	Meta m;
	CHECK(c.readMeta(m) == Cache::Status::Ok);   // the manifest is parseable
	CHECK(!m.information.present && !m.featured.present && m.contents.present);
	Bodies r;
	CHECK(c.readBodies(m, r) == Cache::Status::Ok && r.contents == b.contents && !r.information && !r.featured);
	CHECK(onlyCurrent(t.sub("c"), 1, false, false));
	// ... and the next write over it still works (the manifest of the last one was readable).
	CHECK(c.write(makeBodies("f"), kBase, kApp, 11));
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.generation == 2 && m.information.present);
}

static std::string manifestText(const std::string& dir) { return slurp(dir + "/cache.json"); }

static void testCorruption() {
	TempDir t;
	const std::string d = t.sub("cache");
	Cache c(d);
	Bodies b = makeBodies("orig");
	const std::string contentsFile = d + "/contents.1.json", infoFile = d + "/information.1.json";
	auto fresh = [&] {
		c.invalidate();
		CHECK(c.write(b, kBase, kApp, 1000));
	};
	auto status = [&] {
		Meta m;
		Cache::Status s = c.readMeta(m);
		if (s != Cache::Status::Ok) return s;
		Bodies r;
		return c.readBodies(m, r);
	};
	fresh();
	CHECK(status() == Cache::Status::Ok);
	// Truncated body, same-size edit, empty body, missing body.
	std::string body = slurp(contentsFile);
	spit(contentsFile, body.substr(0, body.size() / 2));
	CHECK(status() == Cache::Status::Corrupt);
	fresh();
	std::string edited = body;
	edited[10] = edited[10] == 'x' ? 'y' : 'x';
	spit(contentsFile, edited);
	CHECK(status() == Cache::Status::Corrupt);   // same size, caught by the hash
	fresh();
	spit(contentsFile, "");
	CHECK(status() == Cache::Status::Corrupt);
	fresh();
	fs::remove(contentsFile);
	CHECK(status() == Cache::Status::Corrupt);
	fresh();
	fs::remove(infoFile);
	CHECK(status() == Cache::Status::Corrupt);   // the manifest lists it, so it must exist
	// A file far larger than the limit is refused without being read into memory (TooLarge counts as damaged: it can
	// never be a valid body).
	fresh();
	spit(contentsFile, std::string(oscapi::kMaxContentsBytes + 1, 'a'));
	CHECK(status() == Cache::Status::Corrupt);
	fresh();
	// Manifest damage: truncated, empty, garbage, wrong types, oversized, missing contents entry, bad hash text.
	const std::string man = manifestText(d);
	for (size_t cut : {size_t(0), size_t(1), man.size() / 2, man.size() - 3}) {
		spit(d + "/cache.json", man.substr(0, cut));
		CHECK(status() == Cache::Status::Corrupt);
	}
	spit(d + "/cache.json", "not json at all");
	CHECK(status() == Cache::Status::Corrupt);
	spit(d + "/cache.json", "[1,2,3]");
	CHECK(status() == Cache::Status::Corrupt);
	spit(d + "/cache.json", man + std::string(kMaxManifestBytes, ' '));
	CHECK(status() == Cache::Status::Corrupt);   // above the manifest cap, even though it is valid JSON
	auto replace = [&](const std::string& from, const std::string& to) {
		std::string s = man;
		size_t p = s.find(from);
		CHECK(p != std::string::npos);
		if (p != std::string::npos) s.replace(p, from.size(), to);
		spit(d + "/cache.json", s);
	};
	const std::string fmt = "\"format\": " + std::to_string(kFormat);
	replace(fmt, "\"format\": \"2\"");
	CHECK(status() == Cache::Status::Corrupt);
	replace("\"fetchedAt\": 1000", "\"fetchedAt\": -5");
	CHECK(status() == Cache::Status::Corrupt);
	replace("\"fetchedAt\": 1000", "\"fetchedAt\": 99999999999999");
	CHECK(status() == Cache::Status::Corrupt);
	replace("\"size\": " + std::to_string(b.contents->size()), "\"size\": 99999999");   // above the contents limit
	CHECK(status() == Cache::Status::Corrupt);
	replace("\"size\": " + std::to_string(b.contents->size()), "\"size\": 0");
	CHECK(status() == Cache::Status::Corrupt);
	replace("\"size\": " + std::to_string(b.contents->size()), "\"size\": " + std::to_string(b.contents->size() + 1));
	CHECK(status() == Cache::Status::Corrupt);   // the file is shorter than the manifest says
	{
		std::string s = man;
		size_t p = s.find("\"fnv\"");
		size_t q = s.find('"', s.find(':', p) + 2);
		s.replace(q, 16, "zzzzzzzzzzzzzzzz");
		spit(d + "/cache.json", s);
		CHECK(status() == Cache::Status::Corrupt);
	}
	spit(d + "/cache.json", "{\"format\":2,\"app\":\"x\",\"base\":\"y\",\"fetchedAt\":1,\"generation\":1,\"files\":{}}");
	CHECK(status() == Cache::Status::Corrupt);   // no contents entry
	// A generation that is not a number: the file names are computed from it, so it must be one.
	replace("\"generation\": 1", "\"generation\": \"../x\"");
	CHECK(status() == Cache::Status::Corrupt);
	// A wrong-format manifest is valid but unusable: the decision says Fetch (and the caller replaces it).
	fresh();
	replace(fmt, "\"format\": 99");
	Meta m;
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.format == 99);
	CHECK(!usable(m, kBase));
	// The pre-generation layout (format 1) is incompatible, and the next write removes its files.
	spit(d + "/cache.json", "{\"format\":1,\"app\":\"x\",\"base\":\"" + kBase + "\",\"fetchedAt\":1,\"generation\":4,\"files\":"
	                        "{\"contents\":{\"size\":3,\"fnv\":\"0000000000000000\"}}}");
	spit(d + "/contents.json", "abc");
	spit(d + "/information.json.bak", "abc");
	spit(d + "/featured.json", "abc");
	CHECK(c.readMeta(m) == Cache::Status::Ok && !usable(m, kBase));
	spit(d + "/notours.txt", "keep me");
	uint32_t gen = 0;
	CHECK(c.write(b, kBase, kApp, 5, &gen) && gen == 5);   // numbered past the old manifest's generation
	CHECK(fs::exists(d + "/notours.txt"));                  // files that do not match our patterns are never touched
	fs::remove(d + "/notours.txt");
	CHECK(onlyCurrent(d, 5, true, true));
	// invalidate() clears everything of ours, also leftovers of every kind.
	spit(d + "/contents.3.json.tmp", "x");
	spit(d + "/cache.json.bak", "x");
	spit(d + "/featured.2.json", "x");
	c.invalidate();
	CHECK(fs::is_empty(d));
}

// The manifest falls back to cache.json.bak when cache.json is missing OR damaged, never after an I/O error.
static void testManifestBakFallback() {
	TempDir t;
	const std::string d = t.sub("c");
	Cache c(d);
	Bodies b = makeBodies("keep");
	CHECK(c.write(b, kBase, kApp, 77));
	const std::string man = slurp(d + "/cache.json");
	Meta m;
	Bodies r;
	// Missing cache.json (power cut between the two renames of its replacement): the .bak is used.
	fs::rename(d + "/cache.json", d + "/cache.json.bak");
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.fetchedAt == 77 && c.readBodies(m, r) == Cache::Status::Ok && sameBodies(r, b));
	// Damaged cache.json with a good .bak: the .bak is used too.
	spit(d + "/cache.json", "{{{{");
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.fetchedAt == 77);
	// Both damaged: Corrupt. Both gone: Missing.
	spit(d + "/cache.json.bak", "oops");
	CHECK(c.readMeta(m) == Cache::Status::Corrupt);
	fs::remove(d + "/cache.json");
	fs::remove(d + "/cache.json.bak");
	CHECK(c.readMeta(m) == Cache::Status::Missing);
}

// M2: an I/O error on a read says nothing about the data. Status Unreadable, never Corrupt, so the caller deletes nothing.
static std::string g_failRead;   // reads of paths containing this fail with IoError
static ReadStatus g_failStatus = ReadStatus::IoError;
static ReadStatus tRead(const char* path, std::string& out, size_t maxBytes) {
	if (!g_failRead.empty() && std::string(path).find(g_failRead) != std::string::npos) {
		out.clear();
		return g_failStatus;
	}
	return readFileCapped(path, out, maxBytes);
}

static void testReadErrors() {
	TempDir t;
	const std::string d = t.sub("c");
	FileOps ops;
	ops.read = tRead;
	Cache c(d, ops);
	Bodies b = makeBodies("rd");
	CHECK(c.write(b, kBase, kApp, 5));
	const auto before = listing(d);
	Meta m;
	Bodies r;
	g_failStatus = ReadStatus::IoError;
	g_failRead = "cache.json";
	CHECK(c.readMeta(m) == Cache::Status::Unreadable);
	// ... also with a (stale) .bak present: no fallback after an I/O error.
	spit(d + "/cache.json.bak", slurp(d + "/cache.json"));
	CHECK(c.readMeta(m) == Cache::Status::Unreadable);
	fs::remove(d + "/cache.json.bak");
	g_failRead = "";
	CHECK(c.readMeta(m) == Cache::Status::Ok);
	for (const char* base : {"contents", "information", "featured"}) {
		g_failRead = Cache::bodyName(base, m.generation);
		CHECK(c.readBodies(m, r) == Cache::Status::Unreadable && !r.contents);
	}
	// A body too large for its endpoint (TooLarge) can never be valid: Corrupt, so it is deleted and refetched.
	g_failStatus = ReadStatus::TooLarge;
	CHECK(c.readBodies(m, r) == Cache::Status::Corrupt);
	g_failRead = "cache.json";
	CHECK(c.readMeta(m) == Cache::Status::Corrupt);
	g_failRead = "";
	CHECK(listing(d) == before);   // nothing was deleted by any of it
	CHECK(c.readMeta(m) == Cache::Status::Ok && c.readBodies(m, r) == Cache::Status::Ok && sameBodies(r, b));
}

// ---------------------------------------------------------------------------------------------------------------------
// Fault sweeps. The injected filesystem calls count themselves; from call number `g_cutAt` on they do nothing and fail,
// which leaves the directory exactly as a power cut at that moment would (the data of a tmp file is on disk, and the
// next rename or remove never happened). g_failRename instead fails every rename whose destination contains the text
// (a full card, a flaky card) while everything else works.
static int g_calls = 0, g_cutAt = -1;
static bool g_fat = false; // rename refuses an existing destination, like libfat
static std::string g_failRename;

static bool cut() { return g_cutAt >= 0 && g_calls >= g_cutAt; }
static int tRename(const char* a, const char* b) {
	bool dead = cut();
	g_calls++;
	if (dead) return -1;
	if (g_fat && fs::exists(b)) return -1;
	if (!g_failRename.empty() && std::string(b).find(g_failRename) != std::string::npos) return -1;
	return std::rename(a, b);
}
static int tRemove(const char* p) {
	bool dead = cut();
	g_calls++;
	if (dead) return -1;
	return std::remove(p);
}

static FileOps sweepOps() {
	FileOps o;
	o.rename = tRename;
	o.remove = tRemove;
	return o;
}

// Which committed set a reader sees now: its index in `sets`, -1 for nothing, -2 for anything else (a mix, or damage).
static int observe(const std::string& dir, const std::vector<Bodies>& sets) {
	Cache c(dir);
	Meta m;
	Cache::Status s = c.readMeta(m);
	if (s == Cache::Status::Missing) return -1;
	if (s != Cache::Status::Ok) return -2;
	Bodies r;
	if (c.readBodies(m, r) != Cache::Status::Ok) return -2;
	for (size_t i = 0; i < sets.size(); i++)
		if (sameBodies(r, sets[i])) return (int)i;
	return -2;
}

// One write with the call `cutAt` and every later one failing (-1: uninterrupted). Returns the number of calls made.
static int runWrite(const std::string& dir, const Bodies& b, int64_t at, bool fat, int cutAt) {
	g_fat = fat; g_calls = 0; g_cutAt = cutAt;
	{
		Cache c(dir, sweepOps());
		(void)c.write(b, kBase, kApp, at);
	}
	int n = g_calls;
	g_fat = false; g_cutAt = -1;
	return n;
}

static int callsFor(const std::string& dir, const Bodies& b, bool fat) {
	TempDir copy;
	fs::copy(dir, copy.sub("c"), fs::copy_options::recursive);
	return runWrite(copy.sub("c"), b, 1, fat, -1);
}

// The shared check after a step: the reader sees a committed set, not older than the newest one already seen.
static void expectCommitted(const std::string& dir, const std::vector<Bodies>& sets, int& newest, const char* what, int a, int b) {
	int seen = observe(dir, sets);
	if (seen < 0 || seen < newest) {
		std::printf("  %s (cut at %d, then %d): reader saw %s\n", what, a, b,
		            seen == -1 ? "nothing, a committed cache was lost" : seen == -2 ? "a corrupt or mixed cache" : "an older cache than before");
		failures++;
	}
	if (seen > newest) newest = seen;
}

// Write 2 cut at every call, then write 3 cut at every call (each from the state write 2 left), then an uninterrupted
// write 4 that must recover and leave no garbage. Optional bodies present or absent per step (`pattern` bit i = step i).
static void doubleFaultSweep(bool fat, int pattern) {
	std::vector<Bodies> sets;
	for (int i = 0; i < 4; i++) {
		bool opt = (pattern >> i) & 1;
		sets.push_back(makeBodies("set" + std::to_string(i + 1), opt, opt));
	}
	TempDir base;
	const std::string baseDir = base.sub("c");
	{
		Cache plain(baseDir);
		CHECK(plain.write(sets[0], kBase, kApp, 100));
	}
	const int total2 = callsFor(baseDir, sets[1], fat);
	CHECK(total2 >= 4);
	for (int a = 0; a <= total2; a++) {
		TempDir t2;
		fs::copy(baseDir, t2.sub("c"), fs::copy_options::recursive);
		runWrite(t2.sub("c"), sets[1], 200, fat, a);
		int newest = 0;
		expectCommitted(t2.sub("c"), sets, newest, "after write 2", a, -1);
		const int total3 = callsFor(t2.sub("c"), sets[2], fat);
		for (int bcut = 0; bcut <= total3; bcut++) {
			TempDir t3;
			fs::copy(t2.sub("c"), t3.sub("c"), fs::copy_options::recursive);
			int seenBefore = newest;
			runWrite(t3.sub("c"), sets[2], 300, fat, bcut);
			expectCommitted(t3.sub("c"), sets, seenBefore, "after write 3", a, bcut);
			// Recovery: a normal write succeeds, the reader sees exactly it, and nothing but its files remains.
			Cache again(t3.sub("c"));
			CHECK(again.write(sets[3], kBase, kApp, 400));
			CHECK(observe(t3.sub("c"), sets) == 3);
			Meta m;
			CHECK(again.readMeta(m) == Cache::Status::Ok && onlyCurrent(t3.sub("c"), m.generation, sets[3].information.has_value(), sets[3].featured.has_value()));
		}
	}
}

static void testDoubleFaultSweep() {
	for (bool fat : {false, true})
		for (int pattern : {0xF, 0x5, 0xA}) doubleFaultSweep(fat, pattern);
}

// A single fault from nothing, and from an existing cache: the old cache is read, or the new one, never a mix.
static void testSingleFaultSweep() {
	for (bool fat : {false, true})
		for (bool haveOld : {false, true})
			for (bool optional : {true, false}) {
				const std::vector<Bodies> sets = {makeBodies("old"), makeBodies("new", optional, optional)};
				TempDir base;
				if (haveOld) { Cache plain(base.sub("c")); CHECK(plain.write(sets[0], kBase, kApp, 100)); }
				else fs::create_directories(base.sub("c"));
				const int total = callsFor(base.sub("c"), sets[1], fat);
				for (int a = 0; a <= total; a++) {
					TempDir t;
					fs::copy(base.sub("c"), t.sub("c"), fs::copy_options::recursive);
					runWrite(t.sub("c"), sets[1], 200, fat, a);
					int seen = observe(t.sub("c"), sets);
					if (seen == -2 || (haveOld && seen == -1)) {
						std::printf("  single fault at call %d of %d (fat=%d old=%d): reader saw %s\n", a, total, fat, haveOld,
						            seen == -2 ? "a corrupt mix" : "nothing, the old cache was lost");
						failures++;
					}
				}
			}
}

// Three faults in a row, random cut points (seeded, so a failure reproduces), then recovery.
static void testTripleFaultSample() {
	std::mt19937 rng(0x0517);
	std::vector<Bodies> sets;
	for (int i = 0; i < 5; i++) sets.push_back(makeBodies("s" + std::to_string(i), i % 2 == 0, i % 3 != 1));
	for (int round = 0; round < 150; round++) {
		const bool fat = rng() & 1;
		TempDir t;
		{ Cache plain(t.sub("c")); CHECK(plain.write(sets[0], kBase, kApp, 1)); }
		int newest = 0;
		for (int step = 1; step <= 3; step++) {
			const int total = callsFor(t.sub("c"), sets[step], fat);
			const int cutAt = (int)(rng() % (uint32_t)(total + 1));
			runWrite(t.sub("c"), sets[step], 10 + step, fat, cutAt);
			expectCommitted(t.sub("c"), sets, newest, "triple fault", round, cutAt);
		}
		Cache again(t.sub("c"));
		CHECK(again.write(sets[4], kBase, kApp, 99));
		CHECK(observe(t.sub("c"), sets) == 4);
		Meta m;
		CHECK(again.readMeta(m) == Cache::Status::Ok && onlyCurrent(t.sub("c"), m.generation, sets[4].information.has_value(), sets[4].featured.has_value()));
	}
}

// Failures that are not a crash: a full card fails the same call again and again. The old cache stays readable
// throughout (the reviewer's repro: three failing attempts used to destroy it), and a later success recovers.
static void testRepeatedFailures() {
	for (bool fat : {false, true})
		for (const char* failing : {"contents.", "cache.json"}) {
			const std::vector<Bodies> sets = {makeBodies("old"), makeBodies("fresh")};
			TempDir t;
			const std::string d = t.sub("c");
			{ Cache plain(d); CHECK(plain.write(sets[0], kBase, kApp, 1)); }
			for (int attempt = 1; attempt <= 6; attempt++) {
				g_fat = fat; g_calls = 0; g_cutAt = -1; g_failRename = failing;
				Cache c(d, sweepOps());
				CHECK(!c.write(sets[1], kBase, kApp, 2));
				g_failRename.clear(); g_fat = false;
				if (observe(d, sets) != 0) {
					std::printf("  attempt %d failing '%s' (fat=%d): the old cache is not readable\n", attempt, failing, fat);
					failures++;
				}
			}
			// The garbage of the failed attempts is bounded (each one cleans up what it wrote).
			CHECK(listing(d).size() <= 8);
			Cache c(d);
			uint32_t gen = 0;
			CHECK(c.write(sets[1], kBase, kApp, 3, &gen));
			CHECK(observe(d, sets) == 1);
			CHECK(onlyCurrent(d, gen, true, true));
		}
}

// The clock: a negative fetch time (time() failed) is stored as 0 = unknown, which is never fresh but still usable.
static void testUnknownClock() {
	TempDir t;
	Cache c(t.sub("c"));
	CHECK(c.write(makeBodies("clk"), kBase, kApp, -1));
	Meta m;
	CHECK(c.readMeta(m) == Cache::Status::Ok && m.fetchedAt == 0);
	std::optional<Meta> om = m;
	Policy p;
	p.baseUrl = kBase;
	p.appVersion = kApp;
	CHECK(usable(om, kBase) && !fresh(om, 1'800'000'000, p));
	CHECK(decideSource(om, 1'800'000'000, p) == Source::FetchWithFallback);
	p.offline = true;
	CHECK(decideSource(om, 1'800'000'000, p) == Source::UseCache);
}

// A mix of two fetches, built by hand, is never accepted.
static void testMixIsRejected() {
	TempDir t;
	const std::string d = t.sub("c");
	Cache c(d);
	Bodies a = makeBodies("A"), b = makeBodies("B");
	CHECK(c.write(a, kBase, kApp, 1));
	std::string manA = slurp(d + "/cache.json");
	CHECK(c.write(b, kBase, kApp, 2));
	spit(d + "/cache.json", manA);   // the old manifest over the new bodies, no .bak around
	Meta m;
	Bodies r;
	CHECK(c.readMeta(m) == Cache::Status::Ok);
	CHECK(c.readBodies(m, r) == Cache::Status::Corrupt);   // generation 1's files are gone
	CHECK(!r.contents);
	// Same-generation bodies swapped in from another fetch fail the hash.
	CHECK(c.write(a, kBase, kApp, 3));
	CHECK(c.readMeta(m) == Cache::Status::Ok);
	spit(d + "/" + Cache::bodyName("contents", m.generation), *b.contents);
	CHECK(c.readBodies(m, r) == Cache::Status::Corrupt);
}

// A fault on the first rename of a write that replaces a file must leave the old file readable.
static void testFileOpsFailures() {
	TempDir t;
	const std::string p = t.sub("f.txt");
	spit(p, "old");
	// Plain overwrite on a FAT-like rename: the dance runs, the file is replaced, no bak stays.
	g_fat = true; g_calls = 0; g_cutAt = -1;
	CHECK(writeFileAtomic(p, "new", sweepOps()));
	CHECK(slurp(p) == "new" && !fs::exists(p + ".bak") && !fs::exists(p + ".tmp"));
	// Every cut point leaves old or new, never nothing or half.
	for (int cutAt = 0; cutAt < 8; cutAt++) {
		spit(p, "old");
		fs::remove(p + ".bak");
		g_calls = 0; g_cutAt = cutAt;
		(void)writeFileAtomic(p, "new", sweepOps());
		g_cutAt = -1;
		std::string now = fs::exists(p) ? slurp(p) : (fs::exists(p + ".bak") ? slurp(p + ".bak") : "(none)");
		CHECK(now == "old" || now == "new");
	}
	g_fat = false;
	// A failed write removes its tmp. (A directory in the way of the tmp name makes fopen fail.)
	fs::create_directory(t.sub("d.txt.tmp"));
	CHECK(!writeFileAtomic(t.sub("d.txt"), "x"));
	CHECK(!fs::exists(t.sub("d.txt")));
	// readFileCapped.
	spit(t.sub("big"), std::string(100, 'q'));
	std::string out;
	CHECK(readFileCapped(t.sub("big"), out, 100) == ReadStatus::Ok && out.size() == 100);
	CHECK(readFileCapped(t.sub("big"), out, 99) == ReadStatus::TooLarge && out.empty());
	CHECK(readFileCapped(t.sub("nope"), out, 99) == ReadStatus::Missing);
	spit(t.sub("empty"), "");
	CHECK(readFileCapped(t.sub("empty"), out, 10) == ReadStatus::Ok && out.empty());
	CHECK(readFileCapped(t.path, out, 10) != ReadStatus::Ok);   // a directory is not a file
	// makeDirs.
	CHECK(makeDirs(t.sub("a/b/c")) && fs::is_directory(t.sub("a/b/c")));
	CHECK(makeDirs(t.sub("a/b/c")));   // already there
	CHECK(!makeDirs(t.sub("big/x")));  // a file in the way
	CHECK(!makeDirs(""));
}

int main() {
	alarm(120);
	testDecisionTable();
	testHash();
	testRoundTrip();
	testEmptyOptionalBodies();
	testCachedCatalogRebuildsUrls();
	testCorruption();
	testManifestBakFallback();
	testReadErrors();
	testSingleFaultSweep();
	testDoubleFaultSweep();
	testTripleFaultSample();
	testRepeatedFailures();
	testUnknownClock();
	testMixIsRejected();
	testFileOpsFailures();
	if (failures) { std::printf("cache_test: %d failure(s)\n", failures); return 1; }
	std::puts("cache_test: all passed");
	return 0;
}
