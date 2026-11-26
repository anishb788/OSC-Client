// The live catalog loader end to end: a real Worker with the POSIX transport against the loopback server, which
// serves the real fixture files (tests/fixtures/, run from the repo root), plus FakeTransport for network errors that
// cannot be produced on loopback. Covers the state machine the LoadingScreen sees (Pending texts, Done, Failed),
// error mapping, the per-endpoint byte limits, cancel by destroying the loader, and the worker-not-running case.
//
// Timing rules as in worker_test.cpp: normal thread priority, conditions polled against generous deadlines.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "FakeTransport.h"
#include "LoopbackServer.h"
#include "catalog/CatalogCache.h"
#include "catalog/CatalogLoader.h"
#include "catalog/NetLoader.h"
#include "catalog/OscApi.h"
#include "core/Version.h"
#include "net/HttpPosix.h"
#include "net/Worker.h"

using namespace net;

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::string readFile(const char* path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { std::fprintf(stderr, "cannot read %s (run from the repo root)\n", path); std::exit(2); }
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static Worker::TransportFactory posixFactory() { return [] { return makePosixTransport(); }; }

// Polls like LoadingScreen::draw: drain the worker's results, then poll the loader. Gives up as Pending.
static LoadStatus runUntilDone(Worker& w, CatalogLoader& l, int timeoutMs = 15000, std::vector<std::string>* texts = nullptr) {
	const int64_t end = nowMs() + timeoutMs;
	LoadStatus st;
	while (nowMs() < end) {
		w.drainResults();
		st = l.poll();
		if (texts && st.state == LoadStatus::State::Pending && (texts->empty() || texts->back() != st.text)) texts->push_back(st.text);
		if (st.state != LoadStatus::State::Pending) return st;
		usleep(2000);
	}
	return st;
}

// The user-facing text for a bare error kind.
static std::string msgFor(HttpError e) {
	HttpResult r;
	r.error = e;
	return describe(r);
}

static std::string info, contents, featured;

static void serveFixtures(LoopbackServer& s) {
	s.addRoute("/api/v4/information", info);
	s.addRoute("/api/v4/contents", contents);
	s.addRoute("/api/v4/featured-app", featured);
}

static NetLoaderConfig cfgFor(const LoopbackServer& s) {
	NetLoaderConfig c;
	c.baseUrl = s.url("/api/v4");
	return c;
}

static void testRealFixtures() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	auto l = makeNetworkLoader(&w, cfgFor(srv));
	std::vector<std::string> texts;
	LoadStatus st = runUntilDone(w, *l, 15000, &texts);
	CHECK(st.state == LoadStatus::State::Done);
	CHECK(st.fromNetwork);
	CHECK(!texts.empty() && texts.front() == "Connecting…");
	Catalog c = l->take();
	CHECK(c.apps.size() >= 300 && c.apps.size() <= 310);
	CHECK(c.featured() && c.featured()->slug == "gxgeo");
	CHECK(c.categories.size() >= 5);
	CHECK(c.fetchedAt > 0);
	CHECK(c.find("WiiDonut") != nullptr);
	// The catalog can be taken once; afterwards the loader says so instead of handing out an empty catalog.
	CHECK(l->poll().state == LoadStatus::State::Failed);
	CHECK(l->take().apps.empty());
	// The three requests, in the documented order, with the JSON Accept header.
	auto reqs = srv.requests();
	CHECK(reqs.size() == 3);
	if (reqs.size() == 3) {
		CHECK(reqs[0].rfind("GET /api/v4/information ", 0) == 0);
		CHECK(reqs[1].rfind("GET /api/v4/contents ", 0) == 0);
		CHECK(reqs[2].rfind("GET /api/v4/featured-app ", 0) == 0);
		CHECK(reqs[1].find("Accept: application/json") != std::string::npos);
	}
	l.reset();
	CHECK(w.pending() == 0);
}

// A missing /information or /featured-app is not fatal: derived categories and the newest app stand in.
static void testOptionalEndpoints() {
	LoopbackServer srv;
	srv.addRoute("/api/v4/contents", contents);   // information and featured-app are 404
	Worker w(posixFactory(), false);
	CHECK(w.start());
	auto l = makeNetworkLoader(&w, cfgFor(srv));
	LoadStatus st = runUntilDone(w, *l);
	CHECK(st.state == LoadStatus::State::Done);
	Catalog c = l->take();
	CHECK(c.apps.size() >= 300);
	CHECK(!c.categories.empty());
	CHECK(c.featured() != nullptr);

	// Garbage in /information and /featured-app is just as harmless.
	LoopbackServer srv2;
	srv2.addRoute("/api/v4/information", "<html>nope</html>");
	srv2.addRoute("/api/v4/contents", contents);
	srv2.addRoute("/api/v4/featured-app", "{\"slug\": 5}", 200);
	auto l2 = makeNetworkLoader(&w, cfgFor(srv2));
	CHECK(runUntilDone(w, *l2).state == LoadStatus::State::Done);
}

static void testFailures() {
	Worker w(posixFactory(), false);
	CHECK(w.start());
	{   // Server error on /contents: the specific status reaches the user.
		LoopbackServer srv;
		srv.addRoute("/api/v4/information", info);
		srv.addRoute("/api/v4/contents", "boom", 503);
		auto l = makeNetworkLoader(&w, cfgFor(srv));
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text.find("HTTP 503") != std::string::npos);
	}
	{   // Bad data: the ApiError message, not a crash and not an empty reason.
		LoopbackServer srv;
		srv.addRoute("/api/v4/information", info);
		srv.addRoute("/api/v4/contents", "<html><body>Welcome to nginx</body></html>");
		auto l = makeNetworkLoader(&w, cfgFor(srv));
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(!st.text.empty() && st.text != "OK");
	}
	{   // Valid JSON of the wrong shape and an empty app list are bad data too.
		for (const char* body : {"{\"a\":1}", "[]", "[{\"name\":\"no slug\"}]"}) {
			LoopbackServer srv;
			srv.addRoute("/api/v4/contents", body);
			auto l = makeNetworkLoader(&w, cfgFor(srv));
			LoadStatus st = runUntilDone(w, *l);
			CHECK(st.state == LoadStatus::State::Failed);
			CHECK(!st.text.empty());
		}
	}
	{   // An oversized body is refused from its header, before anything is buffered: the server never has to send it.
		LoopbackServer srv;
		srv.addRoute("/api/v4/information", info);
		srv.addRawRoute("/api/v4/contents", "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(oscapi::kMaxContentsBytes + 1) +
		                                       "\r\nConnection: close\r\n\r\nxxxx");
		auto l = makeNetworkLoader(&w, cfgFor(srv));
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text == msgFor(HttpError::TooLarge));
	}
	{   // The same bound applies without a Content-Length (chunked): the read stops at the limit.
		LoopbackServer srv;
		srv.addRoute("/api/v4/information", info);
		srv.addRawRoute("/api/v4/contents", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
		                                       std::string("400000\r\n") + patternBody(0x400000) + "\r\n0\r\n\r\n");
		auto l = makeNetworkLoader(&w, cfgFor(srv));
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text == msgFor(HttpError::TooLarge));
	}
	{   // Nothing listening: a connect error, quickly (information failing at the network level is fatal at once).
		NetLoaderConfig c;
		c.baseUrl = "http://127.0.0.1:1/api/v4";
		auto l = makeNetworkLoader(&w, c);
		int64_t t0 = nowMs();
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text == msgFor(HttpError::Connect));
		CHECK(nowMs() - t0 < 5000);
	}
	{   // Redirect to https: the TLS message.
		LoopbackServer srv;
		srv.addRawRoute("/api/v4/information", "HTTP/1.1 302 Found\r\nLocation: https://example.org/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		auto l = makeNetworkLoader(&w, cfgFor(srv));
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text == msgFor(HttpError::TlsRequired));
	}
	{   // A server that drips and stalls is cut off by the total timeout, and says so.
		LoopbackServer srv;
		srv.addRoute("/api/v4/information", info);
		srv.addStallRoute("/api/v4/contents");
		NetLoaderConfig c = cfgFor(srv);
		c.totalTimeoutMs = 400;
		auto l = makeNetworkLoader(&w, c);
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text == msgFor(HttpError::Timeout));
	}
	CHECK(w.pending() == 0);
}

// Errors only a transport fake can produce, and the "one failed request, not three" rule.
static void testFakeTransportErrors() {
	for (HttpError e : {HttpError::NoNetwork, HttpError::Dns}) {
		FakeClock clock;
		std::atomic<int> resolves{0};
		Worker w([&] {
			auto t = std::make_unique<FakeTransport>(clock, std::vector<FakeScript>{});
			t->resolveError = e;
			return t;
		}, false);
		CHECK(w.start());
		NetLoaderConfig c;
		c.baseUrl = "http://example.invalid/api/v4";
		auto l = makeNetworkLoader(&w, c);
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == LoadStatus::State::Failed);
		CHECK(st.text == msgFor(e));
		CHECK(w.pending() == 0);
		(void)resolves;
	}
}

// Destroying the loader mid-fetch (B on a Refresh, Retry, Exit) cancels the job; the worker stays usable.
static void testCancel() {
	LoopbackServer srv;
	serveFixtures(srv);
	srv.addStallRoute("/stall/api/v4/contents");
	srv.addRoute("/stall/api/v4/information", info);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	NetLoaderConfig stall;
	stall.baseUrl = srv.url("/stall/api/v4");
	auto l = makeNetworkLoader(&w, stall);
	int64_t end = nowMs() + 5000;
	while (nowMs() < end && srv.requests().size() < 2) { w.drainResults(); usleep(2000); }
	CHECK(srv.requests().size() == 2);   // /contents is now stalled on the server
	CHECK(l->poll().state == LoadStatus::State::Pending);
	int64_t t0 = nowMs();
	l.reset();
	end = nowMs() + 5000;
	while (nowMs() < end && w.pending() > 0) { w.drainResults(); usleep(2000); }
	CHECK(w.pending() == 0);
	CHECK(nowMs() - t0 < 1000);

	// Retry: a new loader on the same worker succeeds right away.
	auto again = makeNetworkLoader(&w, cfgFor(srv));
	CHECK(runUntilDone(w, *again).state == LoadStatus::State::Done);

	// Destroying a loader whose job already finished (result queued, never polled) is fine too.
	auto late = makeNetworkLoader(&w, cfgFor(srv));
	end = nowMs() + 10000;
	while (nowMs() < end && w.pending() > 0) usleep(2000);
	late.reset();
	w.drainResults();   // the posted closure runs after the loader is gone
	CHECK(w.pending() == 0);
}

// Stopping the worker with a load in flight (Exit mid-load) ends quickly and leaves the loader safe to destroy.
static void testStopMidLoad() {
	LoopbackServer srv;
	srv.addRoute("/api/v4/information", info);
	srv.addStallRoute("/api/v4/contents");
	auto w = std::make_unique<Worker>(posixFactory(), false);
	CHECK(w->start());
	auto l = makeNetworkLoader(w.get(), cfgFor(srv));
	int64_t end = nowMs() + 5000;
	while (nowMs() < end && srv.requests().size() < 2) usleep(2000);
	int64_t t0 = nowMs();
	w->stop();
	CHECK(nowMs() - t0 < 1000);
	l.reset();   // cancel() after stop() is harmless
	w.reset();
}

static NetLoaderConfig deadCfg() {
	NetLoaderConfig c;
	c.baseUrl = "http://127.0.0.1:1/x";
	c.totalTimeoutMs = 1000;
	return c;
}

static void testWorkerNotRunning() {
	const std::string expect = "Couldn't start the network worker.";
	{
		Worker never(posixFactory(), false);   // never started
		auto l = makeNetworkLoader(&never, deadCfg());
		LoadStatus st = l->poll();
		CHECK(st.state == LoadStatus::State::Failed && st.text == expect);
	}
	{
		auto l = makeNetworkLoader(nullptr, deadCfg());
		CHECK(l->poll().state == LoadStatus::State::Failed);
	}
	{   // Stopped after being started: submit() returns 0 inside the loader.
		Worker w(posixFactory(), false);
		CHECK(w.start());
		w.stop();
		auto l = makeNetworkLoader(&w, deadCfg());
		LoadStatus st = l->poll();
		CHECK(st.state == LoadStatus::State::Failed && st.text == expect);
	}
	auto f = makeFailedLoader("reason");
	LoadStatus st = f->poll();
	CHECK(st.state == LoadStatus::State::Failed && st.text == "reason");
}

// makeCatalogLoader's choice: mock only when asked, the fail hook never reaches the network, and a failure is never
// papered over with mock data.
static void testLoaderChoice() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	LoaderOptions o;
	o.mockDelayMs = 0;
	o.worker = &w;
	o.apiBase = srv.url("/api/v4");
	auto count = [&](LoaderOptions lo, int attempt, LoadStatus::State want) {
		auto l = makeCatalogLoader(lo, attempt);
		LoadStatus st = runUntilDone(w, *l);
		CHECK(st.state == want);
		return st;
	};
	for (auto mode : {LoaderOptions::Mode::Startup, LoaderOptions::Mode::Refresh}) {
		o.mode = mode;
		const size_t before = srv.requests().size();
		// Live by default (both modes fetch).
		auto l = makeCatalogLoader(o, 0);
		CHECK(runUntilDone(w, *l).state == LoadStatus::State::Done);
		CHECK(l->take().apps.size() >= 300);
		CHECK(srv.requests().size() == before + 3);
		// mock: the compiled-in catalog, no request.
		LoaderOptions m = o;
		m.mock = true;
		auto ml = makeCatalogLoader(m, 0);
		CHECK(runUntilDone(w, *ml).state == LoadStatus::State::Done);
		CHECK(srv.requests().size() == before + 3);
		// fail hook: failing, and no request. Startup only fails the first attempt, refresh always.
		LoaderOptions f = o;
		f.failStartup = f.failRefresh = true;
		count(f, 0, LoadStatus::State::Failed);
		count(f, 1, mode == LoaderOptions::Mode::Startup ? LoadStatus::State::Done : LoadStatus::State::Failed);
	}
	// A dead server fails (the mock is never a fallback), also when the mock flag is off and the fail hook is not set.
	o.mode = LoaderOptions::Mode::Startup;
	o.apiBase = "http://127.0.0.1:1/api/v4";
	LoadStatus st = count(o, 0, LoadStatus::State::Failed);
	CHECK(!st.text.empty());
	// offline with no cache directory has nothing to read.
	o.offline = true;
	count(o, 0, LoadStatus::State::Failed);
	// No worker: the specific message.
	o.offline = false;
	o.worker = nullptr;
	st = count(o, 0, LoadStatus::State::Failed);
	CHECK(st.text == "Couldn't start the network worker.");
}


// ---------------------------------------------------------------------------------------------------------------------
// The SD cache (Increment 10), end to end: the real worker, the loopback server for "network up" and a dead port for
// "network down", an injected clock, and a temp directory as <dataDir>/cache.
namespace fs = std::filesystem;

struct CacheEnv {
	std::string dir;
	int64_t clock = 1'800'000'000;
	CacheEnv() {
		char tmpl[] = "/tmp/openshop-nl-XXXXXX";
		dir = std::string(mkdtemp(tmpl)) + "/cache";
	}
	~CacheEnv() { std::error_code ec; fs::remove_all(fs::path(dir).parent_path(), ec); }
	std::string file(const char* n) const { return dir + "/" + n; }
};

static const std::string kDeadBase = "http://127.0.0.1:1/api/v4";

static NetLoaderConfig cacheCfg(CacheEnv& env, const std::string& base) {
	NetLoaderConfig c;
	c.baseUrl = base;
	c.cacheDir = env.dir;
	c.now = [&env] { return env.clock; };
	return c;
}

// Runs one load to the end and waits for the worker to go idle (the cache write follows the delivery).
struct Outcome {
	LoadStatus st;
	Catalog cat;
};
static Outcome loadOnce(Worker& w, const NetLoaderConfig& cfg) {
	Outcome o;
	auto l = makeNetworkLoader(&w, cfg);
	o.st = runUntilDone(w, *l);
	if (o.st.state == LoadStatus::State::Done) o.cat = l->take();
	int64_t end = nowMs() + 10000;
	while (nowMs() < end && w.pending() > 0) { w.drainResults(); usleep(2000); }
	CHECK(w.pending() == 0);
	return o;
}

// The path of a body of the current generation, e.g. bodyPath(env, "contents") = <dir>/contents.3.json.
static std::string bodyPath(CacheEnv& env, const char* base) {
	catcache::Cache c(env.dir);
	catcache::Meta m;
	if (c.readMeta(m) != catcache::Cache::Status::Ok) return env.file("(no manifest)");
	return env.file(catcache::Cache::bodyName(base, m.generation).c_str());
}

static catcache::Meta readMetaOf(CacheEnv& env, bool* ok = nullptr) {
	catcache::Cache c(env.dir);
	catcache::Meta m;
	bool good = c.readMeta(m) == catcache::Cache::Status::Ok;
	if (ok) *ok = good;
	return m;
}

static void testCacheStartupFlow() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));

	// 1. First start, no cache: fetched from the network, and the cache appears (after delivery, on the worker).
	Outcome a = loadOnce(w, cfg);
	CHECK(a.st.state == LoadStatus::State::Done && a.st.fromNetwork);
	CHECK(!a.cat.offline && a.cat.fetchedAt == env.clock);
	CHECK(srv.requests().size() == 3);
	for (const char* f : {"cache.json", "contents.1.json", "information.1.json", "featured.1.json"}) CHECK(fs::exists(env.file(f)));
	CHECK(readFile(env.file("contents.1.json").c_str()) == contents);
	bool ok = false;
	catcache::Meta m = readMetaOf(env, &ok);
	CHECK(ok && m.generation == 1 && m.fetchedAt == env.clock && m.baseUrl == cfg.baseUrl);

	// 2. Second start within the TTL: no request at all, same catalog, not flagged offline, fetchedAt from the cache.
	env.clock += 600;
	Outcome b = loadOnce(w, cfg);
	CHECK(b.st.state == LoadStatus::State::Done && !b.st.fromNetwork);
	CHECK(srv.requests().size() == 3);
	CHECK(!b.cat.offline && b.cat.fetchedAt == m.fetchedAt);
	CHECK(b.cat.apps.size() == a.cat.apps.size() && b.cat.featured() && b.cat.featured()->slug == a.cat.featured()->slug);
	CHECK(b.cat.categories.size() == a.cat.categories.size());
	CHECK(readMetaOf(env).generation == 1);   // reading does not rewrite

	// 3. Stale (past the TTL): fetches and rewrites, generation up, new timestamp.
	env.clock += 7200;
	Outcome c = loadOnce(w, cfg);
	CHECK(c.st.state == LoadStatus::State::Done && c.st.fromNetwork && !c.cat.offline);
	CHECK(srv.requests().size() == 6);
	m = readMetaOf(env);
	CHECK(m.generation == 2 && m.fetchedAt == env.clock);

	// 4. The TTL override: ttl 0 means always stale.
	NetLoaderConfig zero = cfg;
	zero.ttlSec = 0;
	Outcome d = loadOnce(w, zero);
	CHECK(d.st.fromNetwork && srv.requests().size() == 9);

	// 5. startup with `refresh` (ignoreTtl) fetches even though the cache is fresh.
	NetLoaderConfig forced = cfg;
	forced.ignoreTtl = true;
	Outcome e = loadOnce(w, forced);
	CHECK(e.st.fromNetwork && srv.requests().size() == 12);

	// 6. A cache written by another app version is stale (fetch), another format version or server is ignored.
	NetLoaderConfig other = cfg;
	other.appVersion = "9.9.9";
	Outcome f = loadOnce(w, other);
	CHECK(f.st.fromNetwork && srv.requests().size() == 15);
	CHECK(readMetaOf(env).appVersion == "9.9.9");

	// 7. A clock that jumped back (the stamp is from the future): not fresh, so it fetches.
	env.clock -= 86400;
	Outcome g = loadOnce(w, other);
	CHECK(g.st.fromNetwork && srv.requests().size() == 18);
	CHECK(w.pending() == 0);
}

static void testCacheOfflineFallback() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());

	{   // A cache written for another server is not used for this one (the dead port): an error, not wrong data.
		CacheEnv env;
		Outcome seed = loadOnce(w, cacheCfg(env, srv.url("/api/v4")));
		CHECK(seed.st.fromNetwork);
		env.clock += 5 * 3600;
		Outcome o = loadOnce(w, cacheCfg(env, kDeadBase));
		CHECK(o.st.state == LoadStatus::State::Failed);
	}
	{
		// A server that is up for the seed and then gone: same base URL, connection refused.
		CacheEnv env;
		auto srv2 = std::make_unique<LoopbackServer>();
		serveFixtures(*srv2);
		const std::string base = srv2->url("/api/v4");
		Outcome seed = loadOnce(w, cacheCfg(env, base));
		CHECK(seed.st.fromNetwork);
		const int64_t seeded = env.clock;
		srv2.reset();   // the network is down now
		env.clock += 5 * 3600;
		Outcome o = loadOnce(w, cacheCfg(env, base));
		CHECK(o.st.state == LoadStatus::State::Done && !o.st.fromNetwork);
		CHECK(o.cat.offline);
		CHECK(o.cat.fetchedAt == seeded);
		CHECK(o.cat.apps.size() == seed.cat.apps.size());
		CHECK(readMetaOf(env).generation == 1);   // the cache itself is untouched

		// Refresh with the network down: no stale fallback, a specific error, and the cache stays exactly as it was.
		const std::string before = readFile(env.file("cache.json").c_str());
		NetLoaderConfig rc = cacheCfg(env, base);
		rc.refreshMode = true;
		Outcome r = loadOnce(w, rc);
		CHECK(r.st.state == LoadStatus::State::Failed);
		CHECK(r.st.text == msgFor(HttpError::Connect));
		CHECK(readFile(env.file("cache.json").c_str()) == before);

		// The startup fallback also covers `refresh` (ignoreTtl) when the cache is fresh.
		env.clock = seeded + 60;
		NetLoaderConfig forced = cacheCfg(env, base);
		forced.ignoreTtl = true;
		Outcome fr = loadOnce(w, forced);
		CHECK(fr.st.state == LoadStatus::State::Done && fr.cat.offline && !fr.st.fromNetwork);
	}
	{   // Network down and no cache: the error, with the specific reason.
		CacheEnv env;
		Outcome o = loadOnce(w, cacheCfg(env, kDeadBase));
		CHECK(o.st.state == LoadStatus::State::Failed);
		CHECK(o.st.text == msgFor(HttpError::Connect));
		CHECK(!fs::exists(env.dir));   // a failed fetch writes nothing
	}
	{   // A server that answers with an error or junk, stale cache present: startup shows the cache (offline),
		// Refresh reports the error and leaves the cache alone.
		CacheEnv env;
		LoopbackServer flaky;
		serveFixtures(flaky);
		NetLoaderConfig cfg = cacheCfg(env, flaky.url("/api/v4"));
		Outcome seed = loadOnce(w, cfg);
		CHECK(seed.st.fromNetwork);
		const std::string before = readFile(env.file("cache.json").c_str());
		env.clock += 7200;
		for (int mode = 0; mode < 2; mode++) {
			if (mode == 0) flaky.addRoute("/api/v4/contents", "boom", 503);
			else flaky.addRoute("/api/v4/contents", "<html>nope</html>");
			Outcome o = loadOnce(w, cfg);
			CHECK(o.st.state == LoadStatus::State::Done && o.cat.offline && !o.st.fromNetwork);
			NetLoaderConfig rc = cfg;
			rc.refreshMode = true;
			Outcome r = loadOnce(w, rc);
			CHECK(r.st.state == LoadStatus::State::Failed);
			if (mode == 0) CHECK(r.st.text.find("HTTP 503") != std::string::npos);
			CHECK(readFile(env.file("cache.json").c_str()) == before);
		}
	}
}

static void testCacheRefreshAndOffline() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	loadOnce(w, cfg);
	CHECK(srv.requests().size() == 3);
	const int64_t seeded = env.clock;

	// Refresh within the TTL still fetches and rewrites the cache with the new timestamp.
	env.clock += 30;
	NetLoaderConfig rc = cfg;
	rc.refreshMode = true;
	Outcome r = loadOnce(w, rc);
	CHECK(r.st.state == LoadStatus::State::Done && r.st.fromNetwork && !r.cat.offline);
	CHECK(srv.requests().size() == 6);
	catcache::Meta m = readMetaOf(env);
	CHECK(m.generation == 2 && m.fetchedAt == seeded + 30);

	// --offline: never touches the network, flagged offline, any age.
	env.clock += 10 * 86400;
	NetLoaderConfig oc = cfg;
	oc.offline = true;
	Outcome o = loadOnce(w, oc);
	CHECK(o.st.state == LoadStatus::State::Done && !o.st.fromNetwork && o.cat.offline && o.cat.fetchedAt == seeded + 30);
	CHECK(srv.requests().size() == 6);
	// A Refresh with --offline re-reads the cache (no request either).
	oc.refreshMode = true;
	Outcome ro = loadOnce(w, oc);
	CHECK(ro.st.state == LoadStatus::State::Done && !ro.st.fromNetwork && ro.cat.offline);
	CHECK(srv.requests().size() == 6);

	// --offline with no cache: an error, and still no request.
	CacheEnv empty;
	NetLoaderConfig nc = cacheCfg(empty, srv.url("/api/v4"));
	nc.offline = true;
	Outcome n = loadOnce(w, nc);
	CHECK(n.st.state == LoadStatus::State::Failed && !n.st.text.empty());
	CHECK(srv.requests().size() == 6);
	nc.refreshMode = true;
	CHECK(loadOnce(w, nc).st.state == LoadStatus::State::Failed);
	// Offline without any cache directory configured is the same error.
	NetLoaderConfig nodir;
	nodir.baseUrl = srv.url("/api/v4");
	nodir.offline = true;
	CHECK(loadOnce(w, nodir).st.state == LoadStatus::State::Failed);
	CHECK(srv.requests().size() == 6);
}

// Whatever is on the SD card is untrusted: every kind of damage ends in a clean miss, never a crash.
static void testCacheDamage() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	loadOnce(w, cfg);
	size_t reqs = srv.requests().size();
	const std::string good = readFile(bodyPath(env, "contents").c_str());
	auto spit = [](const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; };

	// Each kind of damage, network up: detected, deleted, replaced by a fresh fetch (and a valid cache again).
	struct Case { const char* name; std::function<void()> damage; };
	std::vector<Case> cases = {
		{"truncated contents", [&] { spit(bodyPath(env, "contents"), good.substr(0, good.size() / 3)); }},
		{"empty contents", [&] { spit(bodyPath(env, "contents"), ""); }},
		{"edited contents (same size)", [&] { std::string s = good; s[good.size() / 2] ^= 1; spit(bodyPath(env, "contents"), s); }},
		{"missing contents", [&] { fs::remove(bodyPath(env, "contents")); }},
		{"oversized contents", [&] { spit(bodyPath(env, "contents"), std::string(oscapi::kMaxContentsBytes + 100, ' ')); }},
		{"garbage manifest", [&] { spit(env.file("cache.json"), "{{{{"); }},
		{"empty manifest", [&] { spit(env.file("cache.json"), ""); }},
		{"truncated information", [&] { spit(bodyPath(env, "information"), "{\"avail"); }},
	};
	for (auto& cs : cases) {
		cs.damage();
		Outcome o = loadOnce(w, cfg);
		if (!(o.st.state == LoadStatus::State::Done && o.st.fromNetwork && o.cat.apps.size() >= 300)) {
			std::printf("  damage case '%s' did not recover\n", cs.name);
			failures++;
		}
		reqs += 3;
		CHECK(srv.requests().size() == reqs);
		// ... and the replacement is a good cache: the next start reads it without the network.
		Outcome again = loadOnce(w, cfg);
		CHECK(again.st.state == LoadStatus::State::Done && !again.st.fromNetwork);
		CHECK(srv.requests().size() == reqs);
	}

	// A manifest and bodies that agree with each other but are not valid catalog data (an edited cache with a fixed
	// manifest): the parse fails, the cache is deleted, and the network is used.
	{
		catcache::Cache c(env.dir);
		catcache::Bodies junk;
		junk.contents = "<html>hello</html>";
		CHECK(c.write(junk, cfg.baseUrl, OPENSHOP_VERSION, env.clock));
		Outcome o = loadOnce(w, cfg);
		CHECK(o.st.state == LoadStatus::State::Done && o.st.fromNetwork);
		reqs += 3;
		CHECK(srv.requests().size() == reqs);
		CHECK(readMetaOf(env).generation == 1);   // the junk generation is gone; the write continued from nothing
	}
	// ... the same with the network down: an error (offline) and the junk is deleted, not retried forever.
	{
		catcache::Cache c(env.dir);
		catcache::Bodies junk;
		junk.contents = "[1,2,3]";
		CHECK(c.write(junk, cfg.baseUrl, OPENSHOP_VERSION, env.clock));
		NetLoaderConfig oc = cfg;
		oc.offline = true;
		CHECK(loadOnce(w, oc).st.state == LoadStatus::State::Failed);
		catcache::Meta m;
		CHECK(c.readMeta(m) == catcache::Cache::Status::Missing);
		CHECK(srv.requests().size() == reqs);
	}
	// A wrong-format cache is ignored, not trusted, and replaced.
	{
		loadOnce(w, cfg);
		reqs += 3;
		std::string man = readFile(env.file("cache.json").c_str());
		const std::string cur = "\"format\": " + std::to_string(catcache::kFormat);
		size_t p = man.find(cur);
		CHECK(p != std::string::npos);
		man.replace(p, cur.size(), "\"format\": " + std::to_string(catcache::kFormat + 1));
		spit(env.file("cache.json"), man);
		Outcome o = loadOnce(w, cfg);
		CHECK(o.st.state == LoadStatus::State::Done && o.st.fromNetwork);
		reqs += 3;
		CHECK(srv.requests().size() == reqs);
		CHECK(readMetaOf(env).format == catcache::kFormat);
	}
	// A cache directory that cannot be created: the load still succeeds, uncached.
	{
		CacheEnv blocked;
		fs::create_directories(fs::path(blocked.dir).parent_path());
		spit(blocked.dir, "i am a file");
		Outcome o = loadOnce(w, cacheCfg(blocked, srv.url("/api/v4")));
		CHECK(o.st.state == LoadStatus::State::Done && o.st.fromNetwork && o.cat.apps.size() >= 300);
	}
}

// Cache write failures (an injected filesystem) never break the load: the catalog is delivered first.
static void testCacheWriteFailureIsHarmless() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	cfg.fileOps.rename = [](const char*, const char*) { return -1; };
	Outcome o = loadOnce(w, cfg);
	CHECK(o.st.state == LoadStatus::State::Done && o.st.fromNetwork && o.cat.apps.size() >= 300);
	bool ok = true;
	readMetaOf(env, &ok);
	CHECK(!ok);   // nothing committed
}

// The status texts the LoadingScreen shows.
static void testCacheStatusText() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	loadOnce(w, cfg);
	auto l = makeNetworkLoader(&w, cfg);
	CHECK(l->poll().text == "Reading cache…");   // from the first frame
	std::vector<std::string> texts;
	CHECK(runUntilDone(w, *l, 15000, &texts).state == LoadStatus::State::Done);
	for (auto& t : texts) CHECK(t == "Reading cache…");
	l.reset();
	w.drainResults();

	// A Refresh fetches whatever the cache says, so it never says "Reading cache…": it shows "Connecting…" from the
	// first frame, while the manifest is read and while the first request is made.
	NetLoaderConfig rc = cfg;
	rc.refreshMode = true;
	auto r = makeNetworkLoader(&w, rc);
	CHECK(r->poll().text == "Connecting…");
	texts.clear();
	CHECK(runUntilDone(w, *r, 15000, &texts).state == LoadStatus::State::Done);
	CHECK(!texts.empty());
	for (auto& t : texts) CHECK(t != "Reading cache…");
	r.reset();
	w.drainResults();

	// Refresh with --offline does read the cache, and says so.
	NetLoaderConfig oc = rc;
	oc.offline = true;
	auto o = makeNetworkLoader(&w, oc);
	CHECK(o->poll().text == "Reading cache…");
	CHECK(runUntilDone(w, *o).state == LoadStatus::State::Done);
	o.reset();
	w.drainResults();
}

// ---------------------------------------------------------------------------------------------------------------------
// Filesystem hooks for the tests below: they count calls, measure how many threads are inside at once (a write that
// overlapped another would show), and can slow every call down so a test can reach "the write step is running".
static std::atomic<int> g_fsCalls{0}, g_fsDepth{0}, g_fsMaxDepth{0}, g_fsDelayMs{0};
static void fsEnter() {
	int d = ++g_fsDepth;
	int m = g_fsMaxDepth.load();
	while (d > m && !g_fsMaxDepth.compare_exchange_weak(m, d)) {}
	g_fsCalls++;
	if (g_fsDelayMs > 0) usleep((useconds_t)g_fsDelayMs * 1000);
}
static int slowRename(const char* a, const char* b) { fsEnter(); int r = std::rename(a, b); g_fsDepth--; return r; }
static int slowRemove(const char* p) { fsEnter(); int r = std::remove(p); g_fsDepth--; return r; }
static void slowFs(NetLoaderConfig& c) {
	c.fileOps.rename = slowRename;
	c.fileOps.remove = slowRemove;
}
static void resetFs(int delayMs) { g_fsCalls = 0; g_fsDepth = 0; g_fsMaxDepth = 0; g_fsDelayMs = delayMs; }

static bool waitFor(const std::function<bool()>& pred, int timeoutMs = 10000) {
	const int64_t end = nowMs() + timeoutMs;
	while (nowMs() < end) {
		if (pred()) return true;
		usleep(1000);
	}
	return pred();
}

// The directory holds a complete, readable cache of the served fixtures and nothing else.
static void expectCompleteCache(CacheEnv& env, uint32_t gen) {
	bool ok = false;
	catcache::Meta m = readMetaOf(env, &ok);
	CHECK(ok && m.generation == gen);
	catcache::Cache c(env.dir);
	catcache::Bodies b;
	CHECK(ok && c.readBodies(m, b) == catcache::Cache::Status::Ok);
	CHECK(b.contents == contents && b.information == info && b.featured == featured);
	size_t files = 0;
	for (auto& e : fs::directory_iterator(env.dir)) {
		files++;
		const std::string n = e.path().filename().string();
		CHECK(n.find(".tmp") == std::string::npos && n.find(".bak") == std::string::npos);
	}
	CHECK(files == 4);
}

// M4: stop() while the cache write is under way. The write is a Background job that stop() cancels, so it ends at the
// next slice boundary and abandons: the directory then holds either no cache at all (nothing committed yet) or a complete
// one (stopped after the commit), never a half-written one. The wait is bounded by one slice.
static void testStopDuringCacheWrite() {
	LoopbackServer srv;
	serveFixtures(srv);
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	slowFs(cfg);
	resetFs(40);
	auto w = std::make_unique<Worker>(posixFactory(), false);
	CHECK(w->start());
	auto l = makeNetworkLoader(w.get(), cfg);
	CHECK(waitFor([&] { return g_fsCalls > 0; }));   // the first rename of the write: a slice is under way
	const int64_t t0 = nowMs();
	w->stop();
	CHECK(nowMs() - t0 < 3000);
	l.reset();
	w.reset();
	resetFs(0);
	bool ok = false;
	readMetaOf(env, &ok);
	if (ok) {
		expectCompleteCache(env, 1);
	} else {
		// Abandoned before the commit: not one file of ours is left behind.
		size_t files = 0;
		if (fs::exists(env.dir)) for (auto& e : fs::directory_iterator(env.dir)) { (void)e; files++; }
		CHECK(files == 0);
	}
	// Either way the next start works: a fresh fetch, or a read of the complete cache.
	Worker w2(posixFactory(), false);
	CHECK(w2.start());
	Outcome again = loadOnce(w2, cacheCfg(env, srv.url("/api/v4")));
	CHECK(again.st.state == LoadStatus::State::Done);
	expectCompleteCache(env, 1);
}

// M4: the loader destroyed (B, Retry, Exit) after it delivered. The write is its own job, so destroying the loader does
// not touch it: it runs to the end.
static void testCancelDuringCacheWrite() {
	LoopbackServer srv;
	serveFixtures(srv);
	CacheEnv env;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	slowFs(cfg);
	resetFs(40);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	auto l = makeNetworkLoader(&w, cfg);
	CHECK(waitFor([&] { return g_fsCalls > 0; }));
	l.reset();   // never polled: not delivered on the main thread yet, so this raises the cancel flag
	CHECK(waitFor([&] { w.drainResults(); return w.pending() == 0; }));
	resetFs(0);
	expectCompleteCache(env, 1);
	// And the cancelled job left nothing that confuses the next start: it is a plain fresh hit.
	Outcome again = loadOnce(w, cacheCfg(env, srv.url("/api/v4")));
	CHECK(again.st.state == LoadStatus::State::Done && !again.st.fromNetwork && srv.requests().size() == 3);
}

// M4: a load queued behind a write. The write is a Background job in slices; the next Catalog job finishes any pending
// write before it reads the cache (so it never reads during one) and the two writes never overlap.
static void testJobQueuedBehindWrite() {
	LoopbackServer srv;
	serveFixtures(srv);
	CacheEnv env;
	Worker w(posixFactory(), false);
	CHECK(w.start());
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	slowFs(cfg);
	resetFs(40);

	// A startup load, B startup load queued while A writes: B finds A's fresh cache and does not fetch.
	auto a = makeNetworkLoader(&w, cfg);
	CHECK(waitFor([&] { w.drainResults(); return g_fsCalls > 0; }));
	auto b = makeNetworkLoader(&w, cfg);
	LoadStatus sa = runUntilDone(w, *a);
	LoadStatus sb = runUntilDone(w, *b);
	CHECK(sa.state == LoadStatus::State::Done && sa.fromNetwork);
	CHECK(sb.state == LoadStatus::State::Done && !sb.fromNetwork);   // read the cache A wrote
	CHECK(srv.requests().size() == 3);
	CHECK(waitFor([&] { w.drainResults(); return w.pending() == 0; }));
	a.reset();
	b.reset();
	expectCompleteCache(env, 1);

	// A Refresh writing (generation 2), and a second Refresh queued behind it: it fetches again and writes generation 3.
	NetLoaderConfig rc = cfg;
	rc.refreshMode = true;
	resetFs(40);
	auto r1 = makeNetworkLoader(&w, rc);
	CHECK(waitFor([&] { w.drainResults(); return g_fsCalls > 0; }));
	auto r2 = makeNetworkLoader(&w, rc);
	CHECK(runUntilDone(w, *r1).state == LoadStatus::State::Done);
	CHECK(runUntilDone(w, *r2).state == LoadStatus::State::Done);
	CHECK(waitFor([&] { w.drainResults(); return w.pending() == 0; }));
	r1.reset();
	r2.reset();
	CHECK(srv.requests().size() == 9);
	CHECK(g_fsMaxDepth.load() <= 1);   // never two filesystem operations at once, i.e. never two writes at once
	resetFs(0);
	expectCompleteCache(env, 3);
}

// M4: an icon-class job runs while a slow cache write is in progress (the point of moving the write out of the
// Catalog job): it finishes between two slices, before the manifest is committed.
struct MarkJob : Job {
	std::atomic<bool>& done;
	std::atomic<bool>& manifestExisted;
	std::string manifest;
	MarkJob(std::atomic<bool>& d, std::atomic<bool>& m, std::string path) : done(d), manifestExisted(m), manifest(std::move(path)) {}
	JobStep step(JobContext&) override {
		manifestExisted = fs::exists(manifest);
		done = true;
		return JobStep::Done;
	}
};

static void testIconRunsDuringCacheWrite() {
	LoopbackServer srv;
	serveFixtures(srv);
	CacheEnv env;
	std::atomic<bool> done{false}, manifestExisted{false};
	Worker w(posixFactory(), false);
	CHECK(w.start());
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	slowFs(cfg);
	resetFs(100);   // every rename/remove takes 100 ms: the write needs well over 300 ms in total
	auto l = makeNetworkLoader(&w, cfg);
	CHECK(waitFor([&] { w.drainResults(); return g_fsCalls > 0; }));   // the first body's rename: the write is under way
	CHECK(w.submit(JobClass::Icon, "icon", std::make_unique<MarkJob>(done, manifestExisted, env.file("cache.json"))) != 0);
	CHECK(waitFor([&] { return done.load(); }, 5000));
	CHECK(!manifestExisted);          // it ran before the write committed...
	CHECK(w.pending() >= 1);          // ...with the write still pending
	CHECK(runUntilDone(w, *l).state == LoadStatus::State::Done);
	CHECK(waitFor([&] { w.drainResults(); return w.pending() == 0; }));
	l.reset();
	resetFs(0);
	expectCompleteCache(env, 1);
}

// M2: an I/O error while reading the cache is "missing this time": nothing is deleted, whatever the mode.
static std::string g_failReadOf;
static ReadStatus failingRead(const char* path, std::string& out, size_t maxBytes) {
	if (!g_failReadOf.empty() && std::string(path).find(g_failReadOf) != std::string::npos) {
		out.clear();
		return ReadStatus::IoError;
	}
	return readFileCapped(path, out, maxBytes);
}

static std::vector<std::string> dirListing(const CacheEnv& env) {
	std::vector<std::string> v;
	for (auto& e : fs::directory_iterator(env.dir)) v.push_back(e.path().filename().string());
	std::sort(v.begin(), v.end());
	return v;
}

static void testCacheReadErrorKeepsCache() {
	Worker w(posixFactory(), false);
	CHECK(w.start());
	for (const char* target : {"cache.json", "contents.", "information.", "featured."}) {
		auto srv = std::make_unique<LoopbackServer>();
		serveFixtures(*srv);
		CacheEnv env;
		NetLoaderConfig cfg = cacheCfg(env, srv->url("/api/v4"));
		loadOnce(w, cfg);
		const auto listed = dirListing(env);
		NetLoaderConfig flaky = cfg;
		flaky.fileOps.read = failingRead;
		g_failReadOf = target;
		// Offline: the error says "try again", not "it was damaged, so it has been deleted".
		NetLoaderConfig oc = flaky;
		oc.offline = true;
		Outcome o = loadOnce(w, oc);
		CHECK(o.st.state == LoadStatus::State::Failed);
		CHECK(o.st.text.find("deleted") == std::string::npos && o.st.text.find("just now") != std::string::npos);
		CHECK(dirListing(env) == listed);
		// Startup, cache fresh, network gone: not served from the cache this time (a connection error), and the files stay.
		srv.reset();
		Outcome f = loadOnce(w, flaky);
		CHECK(f.st.state == LoadStatus::State::Failed && f.st.text == msgFor(HttpError::Connect));
		CHECK(dirListing(env) == listed);
		// The card is back: the very same cache serves the start.
		g_failReadOf.clear();
		Outcome good = loadOnce(w, cfg);
		CHECK(good.st.state == LoadStatus::State::Done && !good.st.fromNetwork);
	}
	g_failReadOf.clear();
}

// Low: time() failing (-1) is stored as "unknown" (0): usable as a fallback, never fresh.
static void testCacheUnknownClock() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	env.clock = -1;
	NetLoaderConfig cfg = cacheCfg(env, srv.url("/api/v4"));
	Outcome a = loadOnce(w, cfg);
	CHECK(a.st.state == LoadStatus::State::Done && a.st.fromNetwork && a.cat.fetchedAt == 0);
	bool ok = false;
	catcache::Meta m = readMetaOf(env, &ok);
	CHECK(ok && m.fetchedAt == 0);   // not -1: the manifest would have been rejected
	// The clock works again: an unknown stamp is never fresh, so the next start fetches ...
	env.clock = 1'800'000'000;
	Outcome b = loadOnce(w, cfg);
	CHECK(b.st.fromNetwork && srv.requests().size() == 6 && b.cat.fetchedAt == env.clock);
	// ... but it is a perfectly good cache for --offline.
	CacheEnv e2;
	e2.clock = -1;
	NetLoaderConfig c2 = cacheCfg(e2, srv.url("/api/v4"));
	loadOnce(w, c2);
	c2.offline = true;
	Outcome o = loadOnce(w, c2);
	CHECK(o.st.state == LoadStatus::State::Done && !o.st.fromNetwork && o.cat.offline && o.cat.fetchedAt == 0);
}

// makeCatalogLoader maps LoaderOptions onto the cache: modes, offline, the TTL override.
static void testCacheViaLoaderOptions() {
	LoopbackServer srv;
	serveFixtures(srv);
	Worker w(posixFactory(), false);
	CHECK(w.start());
	CacheEnv env;
	LoaderOptions o;
	o.worker = &w;
	o.apiBase = srv.url("/api/v4");
	o.cacheDir = env.dir;
	auto run = [&](const LoaderOptions& lo) {
		auto l = makeCatalogLoader(lo, 0);
		LoadStatus st = runUntilDone(w, *l);
		Catalog c;
		if (st.state == LoadStatus::State::Done) c = l->take();
		l.reset();
		int64_t end = nowMs() + 10000;
		while (nowMs() < end && w.pending() > 0) { w.drainResults(); usleep(2000); }
		return std::make_pair(st, std::move(c));
	};
	auto first = run(o);
	CHECK(first.first.fromNetwork && srv.requests().size() == 3);
	auto second = run(o);   // real clock: well inside the TTL
	CHECK(!second.first.fromNetwork && srv.requests().size() == 3 && !second.second.offline);
	LoaderOptions refresh = o;
	refresh.mode = LoaderOptions::Mode::Refresh;
	CHECK(run(refresh).first.fromNetwork && srv.requests().size() == 6);
	LoaderOptions ttl0 = o;
	ttl0.cacheTtlSec = 0;
	CHECK(run(ttl0).first.fromNetwork && srv.requests().size() == 9);
	LoaderOptions forced = o;
	forced.ignoreTtl = true;
	CHECK(run(forced).first.fromNetwork && srv.requests().size() == 12);
	LoaderOptions off = o;
	off.offline = true;
	auto offRes = run(off);
	CHECK(!offRes.first.fromNetwork && offRes.second.offline && srv.requests().size() == 12);
}

int main() {
	alarm(120); // hang guard: a job that ignored cancel would otherwise block stop() / the server join forever
	info = readFile("tests/fixtures/information.json");
	contents = readFile("tests/fixtures/contents.json");
	featured = readFile("tests/fixtures/featured.json");
	testRealFixtures();
	testOptionalEndpoints();
	testFailures();
	testFakeTransportErrors();
	testCancel();
	testStopMidLoad();
	testWorkerNotRunning();
	testLoaderChoice();
	testCacheStartupFlow();
	testCacheOfflineFallback();
	testCacheRefreshAndOffline();
	testCacheDamage();
	testCacheWriteFailureIsHarmless();
	testCacheStatusText();
	testCacheViaLoaderOptions();
	testStopDuringCacheWrite();
	testCancelDuringCacheWrite();
	testJobQueuedBehindWrite();
	testIconRunsDuringCacheWrite();
	testCacheReadErrorKeepsCache();
	testCacheUnknownClock();
	if (failures) { std::printf("netloader_test: %d failure(s)\n", failures); return 1; }
	std::puts("netloader_test: all passed");
	return 0;
}
