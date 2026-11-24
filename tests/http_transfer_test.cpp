// Host tests for the resumable HttpTransfer over a scripted FakeTransport (virtual clock, no sockets, no sleeps).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "FakeTransport.h"
#include "net/Http.h"
#include "net/HttpParse.h"
#include "net/HttpTransfer.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::string makeBody(size_t n, unsigned seed) {
	std::mt19937 rng(seed);
	std::string s(n, 0);
	for (char& c : s) c = static_cast<char>(rng());
	return s;
}

static std::string okResponse(const std::string& body) {
	return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

static std::string chunkedResponse(const std::string& body, size_t chunk) {
	std::string r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
	char hex[32];
	for (size_t i = 0; i < body.size(); i += chunk) {
		size_t n = std::min(chunk, body.size() - i);
		std::snprintf(hex, sizeof hex, "%zx\r\n", n);
		r += hex;
		r += body.substr(i, n);
		r += "\r\n";
	}
	return r + "0\r\n\r\n";
}

static std::string redirectTo(const std::string& loc, int code = 302) {
	return "HTTP/1.1 " + std::to_string(code) + " Found\r\nLocation: " + loc + "\r\nContent-Length: 0\r\n\r\n";
}

struct Run {
	HttpResult res;
	std::string body;
	bool finishedOk = false;
	int steps = 0;
};

static Run runWith(std::vector<FakeScript> scripts, const StepBudget& budget, HttpRequest req = HttpRequest(),
                   HostCache* cache = nullptr) {
	if (req.url.empty()) req.url = "http://example.org/file";
	FakeClock clock;
	FakeTransport tr(clock, std::move(scripts));
	MemorySink sink(req.maxBody);
	HttpTransfer t(req, sink, nullptr, nullptr, tr, cache);
	Run r;
	while (t.step(budget) == HttpTransfer::StepResult::Pending && r.steps < 10000000) r.steps++;
	r.res = t.result();
	r.body = sink.body();
	r.finishedOk = sink.finishedOk();
	CHECK(sink.finished());
	return r;
}

static bool sameResult(const Run& a, const Run& b) {
	return a.res.error == b.res.error && a.res.status == b.res.status && a.res.bytesReceived == b.res.bytesReceived &&
	       a.res.finalUrl == b.res.finalUrl && a.body == b.body && a.finishedOk == b.finishedOk;
}

static void testBudgetsAgree() {
	std::string body = makeBody(20000, 1);
	std::vector<std::vector<FakeScript>> cases;
	cases.push_back({{okResponse(body)}});
	cases.push_back({{chunkedResponse(body, 777)}});
	{
		FakeScript s;
		s.response = "HTTP/1.1 200 OK\r\n\r\n" + body;   // read-until-close
		cases.push_back({s});
	}
	{
		FakeScript s;
		s.response = okResponse(body);
		s.trickle = true;
		s.maxPerRecv = 100;
		cases.push_back({s});
	}
	cases.push_back({{redirectTo("/other")}, {okResponse(body)}});
	cases.push_back({{"HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nabc"}});
	cases.push_back({{"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort"}});   // closed early
	for (auto& c : cases) {
		StepBudget tiny;
		tiny.maxBytes = 1;
		tiny.maxMs = 1000000;
		StepBudget big;
		big.maxBytes = 1 << 20;
		big.maxMs = 1000000;
		Run a = runWith(c, tiny);
		Run b = runWith(c, big);
		CHECK(sameResult(a, b));
		CHECK(a.steps > b.steps || a.res.bytesReceived == 0);
	}
	Run ok = runWith(cases[0], StepBudget());
	CHECK(ok.res.ok() && ok.body == body && ok.finishedOk);
	CHECK(ok.res.bytesReceived == body.size());
	Run red = runWith(cases[4], StepBudget());
	CHECK(red.res.ok() && red.body == body && red.res.finalUrl == "http://example.org/other");
	Run nf = runWith(cases[5], StepBudget());
	CHECK(nf.res.error == HttpError::HttpStatus && nf.res.status == 404 && nf.body.empty() && !nf.finishedOk);
	Run cut = runWith(cases[6], StepBudget());
	CHECK(cut.res.error == HttpError::Io);
}

// A per-step byte budget actually bounds the work done in one step.
static void testByteBudgetBounds() {
	std::string body = makeBody(100000, 2);
	FakeClock clock;
	FakeTransport tr(clock, {{okResponse(body)}});
	MemorySink sink;
	HttpRequest req;
	req.url = "http://example.org/";
	HttpTransfer t(req, sink, nullptr, nullptr, tr);
	StepBudget b;
	b.maxBytes = 4096;
	b.maxMs = 1000000;
	size_t prev = 0;
	int steps = 0;
	bool bounded = true;
	while (t.step(b) == HttpTransfer::StepResult::Pending) {
		if (sink.body().size() - prev > 4096) bounded = false;
		prev = sink.body().size();
		steps++;
	}
	CHECK(bounded);
	CHECK(steps >= 100000 / 4096);
	CHECK(sink.body() == body);
}

static void testInterleaved() {
	std::string bodyA = makeBody(50000, 3), bodyB = makeBody(33333, 4);
	FakeClock clock;
	FakeScript sa;
	sa.response = chunkedResponse(bodyA, 1000);
	sa.trickle = true;
	sa.maxPerRecv = 700;
	FakeScript sb;
	sb.response = okResponse(bodyB);
	sb.maxPerRecv = 333;
	FakeTransport ta(clock, {sa}), tb(clock, {sb});
	MemorySink ma, mb;
	HttpRequest ra, rb;
	ra.url = "http://a.example/x";
	rb.url = "http://b.example/y";
	HttpTransfer a(ra, ma, nullptr, nullptr, ta), b(rb, mb, nullptr, nullptr, tb);
	StepBudget bud;
	bud.maxBytes = 1500;
	bud.maxMs = 5;
	int rounds = 0;
	bool bothMidway = false;
	while (!a.done() || !b.done()) {
		a.step(bud);
		b.step(bud);
		if (!a.done() && !b.done() && !ma.body().empty() && !mb.body().empty()) bothMidway = true;
		CHECK(++rounds < 100000);
	}
	CHECK(bothMidway);
	CHECK(a.result().ok() && b.result().ok());
	CHECK(ma.body() == bodyA && mb.body() == bodyB);
}

static void testPauseDoesNotTimeOut() {
	// Stalled mid-body: stepping with no data eventually times out, but a pause between steps does not count.
	FakeClock clock;
	std::string body = makeBody(200, 5);
	std::string full = okResponse(body);
	FakeScript s;
	s.response = full.substr(0, full.size() - 100);
	s.closeAtEnd = false;
	FakeTransport tr(clock, {s});
	MemorySink sink;
	HttpRequest req;
	req.url = "http://example.org/";   // idle 10 s
	HttpTransfer t(req, sink, nullptr, nullptr, tr);
	StepBudget b;
	b.maxMs = 50;
	for (int i = 0; i < 5; i++) CHECK(t.step(b) == HttpTransfer::StepResult::Pending);   // about 250 ms stepped
	clock.now += 3600u * 1000u;   // the scheduler leaves it alone for an hour
	CHECK(t.step(b) == HttpTransfer::StepResult::Pending);
	tr.append(full.substr(full.size() - 100));
	int n = 0;
	while (t.step(b) == HttpTransfer::StepResult::Pending && ++n < 100) {}
	CHECK(t.done() && t.result().ok());
	CHECK(sink.body() == body);

	// Same stall, never resumed: stepping alone must trip the idle timer, after about idleTimeoutMs of stepping.
	FakeClock c2;
	FakeScript stall;
	stall.response = full.substr(0, full.size() - 100);
	stall.closeAtEnd = false;
	FakeTransport t2(c2, {stall});
	MemorySink s2;
	HttpTransfer x(req, s2, nullptr, nullptr, t2);
	int steps = 0;
	while (x.step(b) == HttpTransfer::StepResult::Pending && ++steps < 100000) {}
	CHECK(x.done() && x.result().error == HttpError::Timeout);
	CHECK(c2.now >= 10000 && c2.now < 10000 + 200);
	CHECK(!s2.finishedOk() && s2.finished());
	CHECK(x.result().bytesReceived < 200);

	// Connect timer: pauses are free, stepping is not.
	FakeClock c3;
	FakeScript slowConnect;
	slowConnect.connectPolls = -1;
	FakeTransport t3(c3, {slowConnect});
	MemorySink s3;
	HttpRequest cr;
	cr.url = "http://example.org/";
	HttpTransfer y(cr, s3, nullptr, nullptr, t3);
	for (int i = 0; i < 3; i++) {
		CHECK(y.step(b) == HttpTransfer::StepResult::Pending);
		c3.now += 60000;
	}
	steps = 0;
	while (y.step(b) == HttpTransfer::StepResult::Pending && ++steps < 100000) {}
	CHECK(y.result().error == HttpError::Timeout && y.result().detail == "connect timeout");

	// Total timeout is wall-clock, so a pause does count.
	FakeClock c4;
	FakeScript st;
	st.response = full.substr(0, full.size() - 100);
	st.closeAtEnd = false;
	FakeTransport t4(c4, {st});
	MemorySink s4;
	HttpRequest tq = req;
	tq.totalTimeoutMs = 60000;
	HttpTransfer z(tq, s4, nullptr, nullptr, t4);
	CHECK(z.step(b) == HttpTransfer::StepResult::Pending);
	c4.now += 61000;
	CHECK(z.step(b) == HttpTransfer::StepResult::Done);
	CHECK(z.result().error == HttpError::Timeout && z.result().detail == "total timeout");
}

static bool exists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

static void testCancel() {
	std::string body = makeBody(80000, 6);
	std::string path = "/tmp/openshop_transfer_test_" + std::to_string(getpid()) + ".bin";
	std::remove(path.c_str());
	{
		FakeClock clock;
		FakeTransport tr(clock, {{okResponse(body)}});
		FileSink sink(path);
		std::atomic<bool> cancel{false};
		HttpRequest req;
		req.url = "http://example.org/big";
		HttpTransfer t(req, sink, nullptr, &cancel, tr);
		StepBudget b;
		b.maxBytes = 2000;
		b.maxMs = 1000000;
		int n = 0;
		while (t.step(b) == HttpTransfer::StepResult::Pending && ++n < 10000) {
			if (sink.bytesWritten() > 20000) cancel = true;   // "the user pressed cancel" mid-body
		}
		CHECK(t.result().error == HttpError::Cancelled);
		CHECK(t.result().bytesReceived < body.size());
		CHECK(!sink.commit());
		CHECK(!exists(path));
	}
	CHECK(!exists(path + ".tmp"));   // the sink's destructor removed it
	CHECK(!exists(path));

	// Cancelled before the first step: nothing is connected.
	FakeClock clock;
	FakeTransport tr(clock, {{okResponse(body)}});
	MemorySink sink;
	std::atomic<bool> cancel{true};
	HttpRequest req;
	req.url = "http://example.org/";
	HttpTransfer t(req, sink, nullptr, &cancel, tr);
	CHECK(t.step() == HttpTransfer::StepResult::Done);
	CHECK(t.result().error == HttpError::Cancelled && tr.connects == 0 && sink.finished() && !sink.finishedOk());

	// And a successful FileSink download commits.
	{
		FakeClock c2;
		FakeTransport t2(c2, {{okResponse(body)}});
		FileSink fs(path);
		HttpRequest r2;
		r2.url = "http://example.org/";
		HttpResult res = httpGet(r2, fs, nullptr, nullptr, t2);
		CHECK(res.ok() && fs.commit() && exists(path) && !exists(path + ".tmp"));
	}
	std::remove(path.c_str());
}

static void testRedirects() {
	std::string body = makeBody(500, 7);
	{
		FakeClock clock;
		FakeTransport tr(clock, {{redirectTo("/b")}, {redirectTo("http://other.example:8080/c?x=1", 301)}, {okResponse(body)}});
		MemorySink sink;
		HttpRequest req;
		req.url = "http://example.org/a";
		HostCache cache;
		HttpResult r = httpGet(req, sink, nullptr, nullptr, tr, &cache);
		CHECK(r.ok() && sink.body() == body);
		CHECK(tr.connects == 3);
		CHECK(tr.connectPorts[0] == 80 && tr.connectPorts[2] == 8080);
		CHECK(tr.requests[0].find("GET /a HTTP/1.1") == 0);
		CHECK(tr.requests[1].find("GET /b HTTP/1.1") == 0);
		CHECK(tr.requests[2].find("GET /c?x=1 HTTP/1.1") == 0 && tr.requests[2].find("Host: other.example:8080") != std::string::npos);
		CHECK(r.finalUrl == "http://other.example:8080/c?x=1");
		CHECK(tr.resolveCalls == 2);   // example.org once (cached for /b), other.example once
		CHECK(!tr.open);
	}
	{
		FakeClock clock;
		FakeTransport tr(clock, {{redirectTo("https://secure.example/")}});
		MemorySink sink;
		HttpRequest req;
		req.url = "http://example.org/";
		HttpResult r = httpGet(req, sink, nullptr, nullptr, tr);
		CHECK(r.error == HttpError::TlsRequired && tr.connects == 1);
	}
	{
		std::vector<FakeScript> loop(10, FakeScript{redirectTo("/again")});
		FakeClock clock;
		FakeTransport tr(clock, loop);
		MemorySink sink;
		HttpRequest req;
		req.url = "http://example.org/";
		HttpResult r = httpGet(req, sink, nullptr, nullptr, tr);
		CHECK(r.error == HttpError::Redirects && tr.connects == 4);   // first fetch + maxRedirects (3)
	}
	{
		FakeClock clock;
		FakeTransport tr(clock, {{redirectTo("ftp://x/")}});
		MemorySink sink;
		HttpRequest req;
		req.url = "http://example.org/";
		CHECK(httpGet(req, sink, nullptr, nullptr, tr).error == HttpError::Parse);
	}
}

static void testSendWouldBlock() {
	std::string body = makeBody(300, 8);
	FakeScript s;
	s.response = okResponse(body);
	s.maxPerSend = 7;
	s.sendBlocks = true;
	HttpRequest req;
	req.url = "http://example.org/path?q=1";
	req.headers.push_back({"X-Test", "yes"});
	httpparse::Url u;
	httpparse::parseUrl(req.url, u);
	std::string wire = httpparse::buildRequest(u, req);

	// Waiting inside the step: the transfer waits for writability itself.
	{
		FakeClock clock;
		FakeTransport tr(clock, {s});
		MemorySink sink;
		HttpResult r = httpGet(req, sink, nullptr, nullptr, tr);
		CHECK(r.ok() && sink.body() == body && tr.requests[0] == wire);
	}
	// Zero time budget: every WouldBlock hands control back to the scheduler, which does the waiting.
	{
		FakeClock clock;
		FakeTransport tr(clock, {s});
		MemorySink sink;
		HttpTransfer t(req, sink, nullptr, nullptr, tr);
		StepBudget b;
		b.maxMs = 0;
		int steps = 0;
		while (t.step(b) == HttpTransfer::StepResult::Pending && ++steps < 100000) tr.wait(true, 1);
		CHECK(t.result().ok() && sink.body() == body && tr.requests[0] == wire);
		CHECK(steps >= (int)wire.size() / 7);
	}
}

static void testErrors() {
	std::string body = makeBody(100, 9);
	{
		FakeClock clock;
		FakeTransport tr(clock, {{okResponse(body)}});
		tr.resolveError = HttpError::Dns;
		MemorySink sink;
		HttpRequest req;
		req.url = "http://nope.example/";
		HttpResult r = httpGet(req, sink, nullptr, nullptr, tr);
		CHECK(r.error == HttpError::Dns && tr.connects == 0 && sink.finished() && !sink.finishedOk());
	}
	{
		FakeScript s;
		s.connectFails = true;
		Run r = runWith({s}, StepBudget());
		CHECK(r.res.error == HttpError::Connect);
	}
	{
		HttpRequest req;
		req.url = "http://example.org/";
		req.maxBody = 50;
		Run r = runWith({{okResponse(body)}}, StepBudget(), req);
		CHECK(r.res.error == HttpError::TooLarge);
		Run c = runWith({{chunkedResponse(body, 10)}}, StepBudget(), req);
		CHECK(c.res.error == HttpError::TooLarge);
	}
	{
		HttpRequest req;
		req.url = "https://example.org/";
		Run r = runWith({}, StepBudget(), req);
		CHECK(r.res.error == HttpError::TlsRequired);
		req.url = "not a url";
		CHECK(runWith({}, StepBudget(), req).res.error == HttpError::Parse);
	}
	{
		Run r = runWith({{"garbage\r\n\r\n"}}, StepBudget());
		CHECK(r.res.error == HttpError::Parse);
	}
}

static void testProgressAndCache() {
	std::string body = makeBody(30000, 10);
	FakeClock clock;
	FakeScript s;
	s.response = okResponse(body);
	s.trickle = true;
	s.maxPerRecv = 1000;
	FakeTransport tr(clock, {s});
	MemorySink sink;
	uint64_t last = 0;
	int calls = 0;
	bool monotonic = true;
	int64_t total = 0;
	HttpRequest req;
	req.url = "http://example.org/";
	HttpResult r = httpGet(req, sink, [&](uint64_t got, int64_t t) {
		if (got < last) monotonic = false;
		last = got;
		total = t;
		calls++;
	}, nullptr, tr);
	CHECK(r.ok() && monotonic && last == body.size() && total == (int64_t)body.size());
	CHECK(calls >= 2 && calls < 30);   // rate-limited: 30 reads over ~30 virtual ms still gives few calls

	HostCache cache;
	uint32_t a = 0;
	CHECK(!cache.find("h", a));
	for (int i = 0; i < 20; i++) cache.put("host" + std::to_string(i), (uint32_t)i);
	CHECK(cache.find("host19", a) && a == 19);
	CHECK(!cache.find("host0", a));   // evicted, the cache is bounded
}

// A sink that burns virtual time inside its callbacks, like a slow SD card write.
struct SlowSink : MemorySink {
	FakeClock& clock;
	uint64_t costMs;
	SlowSink(FakeClock& c, uint64_t cost) : clock(c), costMs(cost) {}
	bool onData(const uint8_t* d, size_t n) override {
		clock.now += costMs;
		return MemorySink::onData(d, n);
	}
};

static void testSinkTimeIsNotIdle() {
	std::string body = makeBody(6000, 11);
	FakeClock clock;
	FakeScript s;
	s.response = okResponse(body);
	s.maxPerRecv = 1000;
	FakeTransport tr(clock, {s});
	SlowSink sink(clock, 500);
	HttpRequest req;
	req.url = "http://example.org/";
	req.idleTimeoutMs = 100;
	HttpResult r = httpGet(req, sink, nullptr, nullptr, tr);
	CHECK(r.ok() && sink.body() == body);
}

static void testAbandoned() {
	std::string body = makeBody(5000, 12);
	FakeClock clock;
	FakeTransport tr(clock, {{okResponse(body)}});
	struct CountSink : MemorySink {
		int finishes = 0;
		bool lastOk = true;
		void onFinish(bool ok) override { finishes++; lastOk = ok; MemorySink::onFinish(ok); }
	} sink;
	{
		HttpRequest req;
		req.url = "http://example.org/";
		HttpTransfer t(req, sink, nullptr, nullptr, tr);
		StepBudget b;
		b.maxBytes = 1000;
		CHECK(t.step(b) == HttpTransfer::StepResult::Pending);
		CHECK(sink.finishes == 0);
	}
	CHECK(sink.finishes == 1 && !sink.lastOk && !tr.open);
	// A finished transfer is not finished a second time by its destructor.
	CountSink sink2;
	{
		FakeTransport tr2(clock, {{okResponse(body)}});
		HttpRequest req;
		req.url = "http://example.org/";
		HttpTransfer t(req, sink2, nullptr, nullptr, tr2);
		while (t.step() == HttpTransfer::StepResult::Pending) {}
	}
	CHECK(sink2.finishes == 1 && sink2.lastOk);
}

static void testHostCacheErase() {
	std::string body = makeBody(100, 13);
	HostCache cache;
	uint32_t a = 0;
	cache.put("a", 1);
	cache.put("b", 2);
	cache.erase("a");
	cache.erase("missing");
	CHECK(!cache.find("a", a) && cache.find("b", a) && a == 2);

	FakeClock clock;
	FakeScript bad;
	bad.connectFails = true;
	FakeTransport tr(clock, {bad, {okResponse(body)}});
	HostCache c2;
	HttpRequest req;
	req.url = "http://example.org/";
	MemorySink s1;
	CHECK(httpGet(req, s1, nullptr, nullptr, tr, &c2).error == HttpError::Connect);
	MemorySink s2;
	CHECK(httpGet(req, s2, nullptr, nullptr, tr, &c2).ok());
	CHECK(tr.resolveCalls == 2);   // the failure evicted the address

	// A connect-phase timeout and a send failure evict it too; a failure after the connect does not.
	FakeScript stall;
	stall.connectPolls = -1;
	FakeTransport tr3(clock, {stall});
	HostCache c3;
	c3.put("example.org", 5);
	req.connectTimeoutMs = 50;
	MemorySink s3;
	CHECK(httpGet(req, s3, nullptr, nullptr, tr3, &c3).error == HttpError::Timeout);
	CHECK(!c3.find("example.org", a));
	FakeTransport tr4(clock, {{"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"}});
	HostCache c4;
	MemorySink s4;
	CHECK(httpGet(req, s4, nullptr, nullptr, tr4, &c4).error == HttpError::HttpStatus);
	CHECK(c4.find("example.org", a));
}

static void testRedirectHeaders() {
	std::string body = makeBody(50, 14);
	FakeClock clock;
	FakeTransport tr(clock, {{redirectTo("/same")}, {redirectTo("http://other.example/x")}, {okResponse(body)}});
	MemorySink sink;
	HttpRequest req;
	req.url = "http://example.org/a";
	req.headers = {{"Authorization", "Bearer secret"}, {"X-Token", "t"}, {"accept", "text/plain"}};
	HttpResult r = httpGet(req, sink, nullptr, nullptr, tr);
	CHECK(r.ok());
	CHECK(tr.requests[1].find("Authorization: Bearer secret") != std::string::npos);   // same host keeps them
	CHECK(tr.requests[2].find("Authorization") == std::string::npos);
	CHECK(tr.requests[2].find("X-Token") == std::string::npos);
	CHECK(tr.requests[2].find("accept: text/plain") != std::string::npos);
	CHECK(tr.requests[2].find("User-Agent:") != std::string::npos);
	// A port change counts as a different origin.
	FakeTransport tr2(clock, {{redirectTo("http://example.org:81/x")}, {okResponse(body)}});
	MemorySink sink2;
	CHECK(httpGet(req, sink2, nullptr, nullptr, tr2).ok());
	CHECK(tr2.requests[1].find("Authorization") == std::string::npos);
}

static void testWaitBoundedByTimers() {
	// The transport wakes spuriously every 200 ms; the idle timer is 500 ms. A wait sized by the whole idle timeout
	// instead of what is left of it would end at 600 ms.
	FakeClock clock;
	FakeScript s;
	s.closeAtEnd = false;
	s.maxWaitMs = 200;
	FakeTransport tr(clock, {s});
	MemorySink sink;
	HttpRequest req;
	req.url = "http://example.org/";
	req.idleTimeoutMs = 500;
	HttpTransfer t(req, sink, nullptr, nullptr, tr);
	StepBudget b;
	b.maxMs = 1 << 30;
	CHECK(t.step(b) == HttpTransfer::StepResult::Done);
	CHECK(t.result().error == HttpError::Timeout && clock.now <= 501);

	FakeClock c2;
	FakeScript st;
	st.connectPolls = -1;
	st.maxWaitMs = 200;
	FakeTransport tr2(c2, {st});
	MemorySink sink2;
	req.connectTimeoutMs = 500;
	HttpTransfer t2(req, sink2, nullptr, nullptr, tr2);
	CHECK(t2.step(b) == HttpTransfer::StepResult::Done);
	CHECK(t2.result().error == HttpError::Timeout && c2.now <= 501);
}

static void testZeroByteSend() {
	FakeClock clock;
	FakeScript s;
	s.sendZero = true;
	FakeTransport tr(clock, {s});
	MemorySink sink;
	HttpRequest req;
	req.url = "http://example.org/";
	req.idleTimeoutMs = 50;
	HttpTransfer t(req, sink, nullptr, nullptr, tr);
	int n = 0;
	while (t.step() == HttpTransfer::StepResult::Pending && ++n < 100000) {}
	CHECK(n < 100000 && t.result().error == HttpError::Timeout);
}

int main() {
	testBudgetsAgree();
	testByteBudgetBounds();
	testInterleaved();
	testPauseDoesNotTimeOut();
	testCancel();
	testRedirects();
	testSendWouldBlock();
	testErrors();
	testProgressAndCache();
	testSinkTimeIsNotIdle();
	testAbandoned();
	testHostCacheErase();
	testRedirectHeaders();
	testWaitBoundedByTimers();
	testZeroByteSend();
	if (failures) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("http_transfer_test: all passed\n");
	return 0;
}
