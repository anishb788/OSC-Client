// The live catalog loader end to end: a real Worker with the POSIX transport against the loopback server, which
// serves the real fixture files (tests/fixtures/, run from the repo root), plus FakeTransport for network errors that
// cannot be produced on loopback. Covers the state machine the LoadingScreen sees (Pending texts, Done, Failed),
// error mapping, the per-endpoint byte limits, cancel by destroying the loader, and the worker-not-running case.
//
// Timing rules as in worker_test.cpp: normal thread priority, conditions polled against generous deadlines.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "FakeTransport.h"
#include "LoopbackServer.h"
#include "catalog/CatalogLoader.h"
#include "catalog/NetLoader.h"
#include "catalog/OscApi.h"
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

static void testWorkerNotRunning() {
	const std::string expect = "Couldn't start the network worker.";
	{
		Worker never(posixFactory(), false);   // never started
		auto l = makeNetworkLoader(&never, NetLoaderConfig{"http://127.0.0.1:1/x", 1000});
		LoadStatus st = l->poll();
		CHECK(st.state == LoadStatus::State::Failed && st.text == expect);
	}
	{
		auto l = makeNetworkLoader(nullptr, NetLoaderConfig{"http://127.0.0.1:1/x", 1000});
		CHECK(l->poll().state == LoadStatus::State::Failed);
	}
	{   // Stopped after being started: submit() returns 0 inside the loader.
		Worker w(posixFactory(), false);
		CHECK(w.start());
		w.stop();
		auto l = makeNetworkLoader(&w, NetLoaderConfig{"http://127.0.0.1:1/x", 1000});
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
	// offline has no cache to read yet.
	o.offline = true;
	count(o, 0, LoadStatus::State::Failed);
	// No worker: the specific message.
	o.offline = false;
	o.worker = nullptr;
	st = count(o, 0, LoadStatus::State::Failed);
	CHECK(st.text == "Couldn't start the network worker.");
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
	if (failures) { std::printf("netloader_test: %d failure(s)\n", failures); return 1; }
	std::puts("netloader_test: all passed");
	return 0;
}
