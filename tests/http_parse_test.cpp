// Host tests for the pure HTTP pieces (no sockets, no SDL): URL parsing, redirect resolution, request building and
// the incremental response parser fed byte by byte and in random splits.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "net/Http.h"
#include "net/HttpParse.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

using namespace httpparse;

struct Outcome {
	ResponseParser::Status st = ResponseParser::Status::NeedMore;
	HttpError err = HttpError::None;
	int status = 0;
	int64_t length = -2;
	bool redirect = false;
	std::string location;
	std::string body;
	bool started = false;
};

enum class Split { Whole, Bytes, Random };

static Outcome run(const std::string& wire, Split split, uint64_t maxBody = 0, bool eof = true, unsigned seed = 1) {
	MemorySink sink;
	ResponseParser p(sink, maxBody);
	std::mt19937 rng(seed);
	size_t i = 0;
	ResponseParser::Status st = ResponseParser::Status::NeedMore;
	while (i < wire.size() && st == ResponseParser::Status::NeedMore) {
		size_t n = split == Split::Whole ? wire.size() : split == Split::Bytes ? 1 : 1 + rng() % 7;
		if (n > wire.size() - i) n = wire.size() - i;
		st = p.feed(reinterpret_cast<const uint8_t*>(wire.data()) + i, n);
		i += n;
	}
	if (st == ResponseParser::Status::NeedMore && eof) st = p.finishEof();
	Outcome o;
	o.st = st;
	o.err = p.error();
	o.status = p.status();
	o.length = p.contentLength();
	o.redirect = p.isRedirect();
	o.location = p.location();
	o.body = sink.body();
	return o;
}

// Every split strategy must agree.
template <class F> static void allSplits(const std::string& wire, F check, uint64_t maxBody = 0, bool eof = true) {
	check(run(wire, Split::Whole, maxBody, eof));
	check(run(wire, Split::Bytes, maxBody, eof));
	for (unsigned seed = 1; seed <= 25; seed++) check(run(wire, Split::Random, maxBody, eof, seed));
}

using St = ResponseParser::Status;

static void testUrl() {
	Url u;
	CHECK(parseUrl("http://hbb1.oscwii.org/api/v4/contents", u) == HttpError::None);
	CHECK(u.host == "hbb1.oscwii.org" && u.port == 80 && u.path == "/api/v4/contents");
	CHECK(parseUrl("HTTP://Example.COM:8080/a?b=c#frag", u) == HttpError::None);
	CHECK(u.host == "example.com" && u.port == 8080 && u.path == "/a?b=c");
	CHECK(parseUrl("http://host", u) == HttpError::None && u.path == "/");
	CHECK(parseUrl("http://host?x=1", u) == HttpError::None && u.path == "/?x=1");
	CHECK(toString(u) == "http://host/?x=1");
	CHECK(parseUrl("http://host:81/p", u) == HttpError::None && toString(u) == "http://host:81/p");
	CHECK(parseUrl("https://hbb1.oscwii.org/x", u) == HttpError::TlsRequired);
	CHECK(parseUrl("HTTPS://h/", u) == HttpError::TlsRequired);
	const char* bad[] = {"", "ftp://h/", "hbb1.oscwii.org/x", "http://", "http:///x", "http://h:/x", "http://h:0/",
	                     "http://h:70000/", "http://h:8a/", "http://u@h/", "http://[::1]/", "http://h/a b",
	                     "http://h/\r\nX: y", "http://ho st/"};
	for (const char* b : bad) CHECK(parseUrl(b, u) == HttpError::Parse);
}

static std::string resolve(const char* base, const char* loc, HttpError* e = nullptr) {
	Url b;
	parseUrl(base, b);
	std::string out;
	HttpError r = resolveLocation(b, loc, out);
	if (e) *e = r;
	return out;
}

static void testResolve() {
	CHECK(resolve("http://h/a/b/c?q=1", "/x/y") == "http://h/x/y");
	CHECK(resolve("http://h/a/b/c?q=1", "d") == "http://h/a/b/d");
	CHECK(resolve("http://h/a/b/c", "../d?z=1") == "http://h/a/d?z=1");
	CHECK(resolve("http://h/a/b/c", "../../../../d") == "http://h/d");
	CHECK(resolve("http://h/a/b/c", "./d/") == "http://h/a/b/d/");
	CHECK(resolve("http://h/a/b/c?q=1", "?r=2") == "http://h/a/b/c?r=2");
	CHECK(resolve("http://h:81/a", "/b") == "http://h:81/b");
	CHECK(resolve("http://h/a", "//other/p") == "http://other/p");
	CHECK(resolve("http://h/a", "http://o/p") == "http://o/p");
	CHECK(resolve("http://h/a", "https://o/p") == "https://o/p"); // parseUrl then reports TlsRequired
	CHECK(resolve("http://h/a", "/b#f") == "http://h/b");
	HttpError e = HttpError::None;
	resolve("http://h/a", "ftp://o/p", &e);
	CHECK(e == HttpError::Parse);
	resolve("http://h/a", "", &e);
	CHECK(e == HttpError::Parse);
}

static void testRequest() {
	Url u;
	parseUrl("http://host:8080/p?q=1", u);
	HttpRequest r;
	r.headers = {{"X-Test", "1"}, {"Bad\r\nName", "x"}, {"X-Inject", "a\r\nEvil: 1"}};
	std::string s = buildRequest(u, r);
	CHECK(s.rfind("GET /p?q=1 HTTP/1.1\r\n", 0) == 0);
	CHECK(s.find("\r\nHost: host:8080\r\n") != std::string::npos);
	CHECK(s.find("\r\nUser-Agent: OpenShop/") != std::string::npos);
	CHECK(s.find("\r\nAccept: */*\r\n") != std::string::npos);
	CHECK(s.find("\r\nAccept-Encoding: identity\r\n") != std::string::npos);
	CHECK(s.find("\r\nConnection: close\r\n") != std::string::npos);
	CHECK(s.find("X-Test: 1\r\n") != std::string::npos);
	CHECK(s.find("Evil") == std::string::npos && s.find("Bad") == std::string::npos);
	CHECK(s.size() > 4 && s.substr(s.size() - 4) == "\r\n\r\n");
	parseUrl("http://host/", u);
	CHECK(buildRequest(u, HttpRequest{}).find("Host: host\r\n") != std::string::npos);
}

static void testContentLength() {
	std::string wire = "HTTP/1.1 200 OK\r\ncontent-LENGTH: 11\r\nContent-Type: text/plain\r\n\r\nhello world";
	allSplits(wire, [](const Outcome& o) {
		CHECK(o.st == St::Done && o.err == HttpError::None && o.status == 200 && o.length == 11);
		CHECK(o.body == "hello world" && !o.redirect);
	});
	// Bytes after the body are ignored (Connection: close, nothing should follow).
	allSplits(wire + "EXTRA", [](const Outcome& o) { CHECK(o.st == St::Done && o.body == "hello world"); });
	// Zero-length body finishes at the end of the headers.
	allSplits("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Done && o.body.empty());
	}, 0, false);
	// Truncated body: connection closed early.
	allSplits("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Io);
	});
	// Bare LF line endings are tolerated.
	allSplits("HTTP/1.1 200 OK\nContent-Length: 2\n\nhi", [](const Outcome& o) { CHECK(o.st == St::Done && o.body == "hi"); });
	// Interim 100 Continue is skipped.
	allSplits("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", [](const Outcome& o) {
		CHECK(o.st == St::Done && o.status == 200 && o.body == "hi");
	});
}

static void testChunked() {
	std::string wire =
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\nContent-Length: 999\r\n\r\n"
		"5\r\nhello\r\n"
		"6;name=val;x=\"y\"\r\n world\r\n"
		"A\r\n0123456789\r\n"
		"0;last=1\r\n"
		"X-Trailer: yes\r\nX-Other: no\r\n"
		"\r\n";
	allSplits(wire, [](const Outcome& o) {
		CHECK(o.st == St::Done && o.err == HttpError::None && o.length == -1);
		CHECK(o.body == "hello world0123456789");
	}, 0, false);
	// Upper-case hex sizes, no trailers.
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nB\r\nhello world\r\n0\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Done && o.body == "hello world");
	}, 0, false);
	// Bad chunk size.
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	});
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	});
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffff\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	});
	// Missing CRLF after chunk data.
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXY\r\n0\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	});
	// Closed mid-chunk.
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Io);
	});
	// Closed before the terminating chunk.
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Io);
	});
	// Unsupported transfer coding.
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	});
}

static void testUntilClose() {
	std::string wire = "HTTP/1.0 200 OK\r\nServer: x\r\n\r\nbody until close";
	allSplits(wire, [](const Outcome& o) {
		CHECK(o.st == St::Done && o.length == -1 && o.body == "body until close");
	});
	// Without the EOF the parser must still be waiting, not done.
	allSplits(wire, [](const Outcome& o) { CHECK(o.st == St::NeedMore && o.body == "body until close"); }, 0, false);
	// EOF before headers finish is a failure.
	allSplits("HTTP/1.1 200 OK\r\nServer: x\r\n", [](const Outcome& o) { CHECK(o.st == St::Failed && o.err == HttpError::Io); });
	allSplits("", [](const Outcome& o) { CHECK(o.st == St::Failed && o.err == HttpError::Io); });
}

static void testTooLarge() {
	// Declared length over the cap fails before any body is read.
	allSplits("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + std::string(100, 'a'), [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::TooLarge && o.body.empty());
	}, 50);
	// Exactly at the cap is fine.
	allSplits("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n" + std::string(50, 'a'), [](const Outcome& o) {
		CHECK(o.st == St::Done && o.body.size() == 50);
	}, 50);
	// Unknown length: caught while streaming.
	allSplits("HTTP/1.0 200 OK\r\n\r\n" + std::string(100, 'a'), [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::TooLarge && o.body.size() <= 50);
	}, 50);
	allSplits("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n32\r\n" + std::string(50, 'a') + "\r\n32\r\n" +
	              std::string(50, 'b') + "\r\n0\r\n\r\n",
	          [](const Outcome& o) { CHECK(o.st == St::Failed && o.err == HttpError::TooLarge); }, 60);
	// MemorySink enforces the same cap on its own.
	MemorySink m(10);
	CHECK(!m.onStart(200, 11) && m.failure() == HttpError::TooLarge);
	MemorySink m2(10);
	const uint8_t d[6] = {1, 2, 3, 4, 5, 6};
	CHECK(m2.onStart(200, -1) && m2.onData(d, 6) && !m2.onData(d, 6) && m2.failure() == HttpError::TooLarge);
}

static void testRedirectAndStatus() {
	const int codes[] = {301, 302, 303, 307, 308};
	for (int c : codes) {
		std::string wire = "HTTP/1.1 " + std::to_string(c) + " Moved\r\nLOCATION: /new/place\r\nContent-Length: 5\r\n\r\nhello";
		allSplits(wire, [c](const Outcome& o) {
			CHECK(o.st == St::Done && o.redirect && o.status == c && o.location == "/new/place");
			CHECK(o.body.empty() && o.err == HttpError::None);
		});
	}
	allSplits("HTTP/1.1 302 Found\r\nLocation: https://hbb1.oscwii.org/x\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Done && o.redirect && o.location == "https://hbb1.oscwii.org/x");
		Url base;
		parseUrl("http://hbb1.oscwii.org/x", base);
		std::string abs;
		CHECK(resolveLocation(base, o.location, abs) == HttpError::None);
		Url t;
		CHECK(parseUrl(abs, t) == HttpError::TlsRequired);
	}, 0, false);
	allSplits("HTTP/1.1 301 Moved\r\nLocation: http://other:8080/a\r\n\r\n", [](const Outcome& o) {
		Url base, t;
		parseUrl("http://h/", base);
		std::string abs;
		CHECK(o.redirect && resolveLocation(base, o.location, abs) == HttpError::None && parseUrl(abs, t) == HttpError::None);
		CHECK(t.host == "other" && t.port == 8080 && t.path == "/a");
	}, 0, false);
	// Redirect status with no Location is just an HTTP error.
	allSplits("HTTP/1.1 302 Found\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Done && !o.redirect && o.err == HttpError::HttpStatus && o.status == 302);
	}, 0, false);
	allSplits("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found", [](const Outcome& o) {
		CHECK(o.st == St::Done && o.err == HttpError::HttpStatus && o.status == 404 && o.body.empty());
	}, 0, false);
	allSplits("HTTP/1.1 500 Oops\r\n\r\n", [](const Outcome& o) { CHECK(o.err == HttpError::HttpStatus && o.status == 500); }, 0, false);
}

static void testMalformed() {
	const char* bad[] = {
		"garbage\r\n\r\n",
		"HTTP/2 200 OK\r\n\r\n",
		"HTTP/1.1 2x0 OK\r\n\r\n",
		"HTTP/1.1 200OK\r\n\r\n",
		"HTTP/1.1 200 OK\r\nNoColonHere\r\n\r\n",
		"HTTP/1.1 200 OK\r\nX-A: 1\r\n folded\r\n\r\n",       // obsolete line folding is rejected
		"HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n",
		"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
		"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
		"HTTP/1.1 200 OK\r\nBad Name: 1\r\n\r\n",
	};
	for (const char* b : bad)
		allSplits(b, [](const Outcome& o) { CHECK(o.st == St::Failed && o.err == HttpError::Parse); });
	// Duplicate but equal Content-Length is fine.
	allSplits("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", [](const Outcome& o) {
		CHECK(o.st == St::Done && o.body == "x");
	});
	// A header line that never ends is cut off.
	allSplits("HTTP/1.1 200 OK\r\nX: " + std::string(20000, 'a') + "\r\n\r\n", [](const Outcome& o) {
		CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	});
}

// A sink that refuses data reports its own failure through the parser.
struct RefusingSink : HttpSink {
	bool onStart(int, int64_t) override { return true; }
	bool onData(const uint8_t*, size_t) override { return false; }
	void onFinish(bool) override {}
};

static void testSinkRefusal() {
	RefusingSink s;
	ResponseParser p(s);
	std::string w = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
	CHECK(p.feed(reinterpret_cast<const uint8_t*>(w.data()), w.size()) == St::Failed && p.error() == HttpError::Io);
	// Once failed, further input keeps reporting failure.
	CHECK(p.feed(reinterpret_cast<const uint8_t*>("x"), 1) == St::Failed);
}

static void testFileSink() {
	std::string path = "/tmp/openshop_http_parse_test_" + std::to_string(getpid()) + ".bin";
	std::remove(path.c_str());
	{
		FileSink f(path);
		const uint8_t d[3] = {'a', 'b', 'c'};
		CHECK(f.onStart(200, 3) && f.onData(d, 3));
		f.onFinish(true);
		std::FILE* t = std::fopen(f.tmpPath().c_str(), "rb");
		CHECK(t != nullptr);
		if (t) std::fclose(t);
		CHECK(f.commit());
		t = std::fopen(f.tmpPath().c_str(), "rb");
		CHECK(t == nullptr); // the tmp was renamed away
		std::FILE* r = std::fopen(path.c_str(), "rb");
		char buf[4] = {};
		CHECK(r && std::fread(buf, 1, 3, r) == 3 && std::string(buf) == "abc");
		if (r) std::fclose(r);
	}
	// A second download replaces the target; an aborted one leaves the target and no tmp.
	{
		FileSink f(path);
		const uint8_t d[1] = {'z'};
		CHECK(f.onStart(200, 1) && f.onData(d, 1));
		f.onFinish(false);
		CHECK(!f.commit());
		f.abort();
		CHECK(std::fopen((path + ".tmp").c_str(), "rb") == nullptr);
		std::FILE* r = std::fopen(path.c_str(), "rb");
		char c = 0;
		CHECK(r && std::fread(&c, 1, 1, r) == 1 && c == 'a');
		if (r) std::fclose(r);
	}
	// Destructor without commit removes the tmp.
	std::string tmp;
	{
		FileSink f(path);
		CHECK(f.onStart(200, -1));
		tmp = f.tmpPath();
	}
	CHECK(std::fopen(tmp.c_str(), "rb") == nullptr);
	// Unwritable location: onStart fails.
	FileSink bad("/nonexistent-dir/x");
	CHECK(!bad.onStart(200, 1));
	std::remove(path.c_str());
}

static std::string slurp(const std::string& p) {
	std::FILE* f = std::fopen(p.c_str(), "rb");
	if (!f) return "<missing>";
	std::string s;
	char buf[256];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
	std::fclose(f);
	return s;
}

static bool fileExists(const std::string& p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0;
}

static void writeFile(const std::string& p, const std::string& c) {
	std::FILE* f = std::fopen(p.c_str(), "wb");
	if (f) {
		std::fwrite(c.data(), 1, c.size(), f);
		std::fclose(f);
	}
}

// Rename that behaves like libfat: it refuses to replace an existing file. failOn counts calls (1-based) that fail
// regardless, to simulate a card error on the second rename.
static int gRenameCalls = 0;
static int gFailOn = 0;
static int fatRename(const char* from, const char* to) {
	gRenameCalls++;
	if (gRenameCalls == gFailOn) return -1;
	if (fileExists(to)) return -1;
	return std::rename(from, to);
}

static bool writeTmp(FileSink& f, const char* data) {
	size_t n = std::strlen(data);
	bool ok = f.onStart(200, static_cast<int64_t>(n)) && f.onData(reinterpret_cast<const uint8_t*>(data), n);
	f.onFinish(ok);
	return ok;
}

static void testFileSinkReplace() {
	std::string path = "/tmp/openshop_http_parse_replace_" + std::to_string(getpid()) + ".bin";
	FileOps ops;
	ops.rename = fatRename;
	std::remove(path.c_str());
	std::remove((path + ".bak").c_str());

	// Rename over an existing file fails first, the .bak fallback succeeds and leaves no .bak behind.
	writeFile(path, "old");
	writeFile(path + ".bak", "stale");
	{
		FileSink f(path);
		f.setFileOps(ops);
		gRenameCalls = 0;
		gFailOn = 0;
		CHECK(writeTmp(f, "new"));
		CHECK(f.commit());
		CHECK(slurp(path) == "new" && !fileExists(path + ".bak") && !fileExists(path + ".tmp"));
		// A second commit() is a no-op failure and must not disturb the target.
		CHECK(!f.commit());
		CHECK(slurp(path) == "new");
	}

	// The tmp-into-place rename fails (call 3: plain, target->bak, tmp->target): the old target is restored intact.
	{
		FileSink f(path);
		f.setFileOps(ops);
		gRenameCalls = 0;
		gFailOn = 3;
		CHECK(writeTmp(f, "newer"));
		CHECK(!f.commit());
		CHECK(slurp(path) == "new" && !fileExists(path + ".bak"));
	}
	CHECK(!fileExists(path + ".tmp"));   // the destructor removed the failed tmp

	// Moving the old target aside fails: nothing is lost.
	{
		FileSink f(path);
		f.setFileOps(ops);
		gRenameCalls = 0;
		gFailOn = 2;
		CHECK(writeTmp(f, "x"));
		CHECK(!f.commit());
		CHECK(slurp(path) == "new");
	}

	// onFinish(true) without the file ever having been opened is not a success.
	{
		FileSink f(path);
		f.onFinish(true);
		CHECK(!f.commit());
		CHECK(slurp(path) == "new");
	}
	std::remove(path.c_str());
	std::remove((path + ".bak").c_str());
}

static void testMemorySinkLimits() {
	// A Content-Length bomb against the default sink: no allocation attempt, no throw, TooLarge.
	std::string bomb = "HTTP/1.1 200 OK\r\nContent-Length: 900000000000000\r\n\r\nabc";
	bool threw = false;
	try {
		MemorySink sink;
		ResponseParser p(sink);
		CHECK(p.feed(reinterpret_cast<const uint8_t*>(bomb.data()), bomb.size()) == St::Failed);
		CHECK(p.error() == HttpError::TooLarge && sink.failure() == HttpError::TooLarge);
		// Explicitly unlimited sinks must not reserve what the server claims either.
		MemorySink unlimited(0);
		ResponseParser p2(unlimited);
		CHECK(p2.feed(reinterpret_cast<const uint8_t*>(bomb.data()), bomb.size()) != St::Failed);
		CHECK(unlimited.body() == "abc" && unlimited.body().capacity() < (1u << 21));
	} catch (...) {
		threw = true;
	}
	CHECK(!threw);
	// The default limit also applies to a chunked body that never declares a length.
	MemorySink sink;
	std::vector<uint8_t> blk(1 << 20, 'a');
	CHECK(sink.onStart(200, -1));
	bool ok = true;
	for (int i = 0; i < 9 && ok; i++) ok = sink.onData(blk.data(), blk.size());
	CHECK(!ok && sink.failure() == HttpError::TooLarge);
}

static void testInterimCap() {
	std::string wire;
	for (int i = 0; i < 8; i++) wire += "HTTP/1.1 100 Continue\r\n\r\n";
	std::string ok = wire + "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
	CHECK(run(ok, Split::Whole).st == St::Done);
	std::string bad = wire + "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx";
	Outcome o = run(bad, Split::Whole);
	CHECK(o.st == St::Failed && o.err == HttpError::Parse);
	CHECK(run(bad, Split::Bytes).err == HttpError::Parse);
}

// Deterministic fuzzing: whatever the bytes are, the parser must stop cleanly, never grow without bound and stay
// failed once failed. Run under SAN=1 this also catches memory errors.
static void fuzzOne(const std::string& wire, std::mt19937& rng, uint64_t maxBody) {
	MemorySink sink(maxBody);
	ResponseParser p(sink, maxBody);
	size_t i = 0;
	St st = St::NeedMore;
	while (i < wire.size() && st == St::NeedMore) {
		size_t n = 1 + rng() % 64;
		if (rng() % 8 == 0) n = 1;
		if (n > wire.size() - i) n = wire.size() - i;
		st = p.feed(reinterpret_cast<const uint8_t*>(wire.data()) + i, n);
		i += n;
	}
	if (st == St::NeedMore) st = p.finishEof();
	CHECK(st != St::NeedMore);
	CHECK(sink.body().size() <= wire.size());
	CHECK(p.bodyBytes() == sink.body().size());
	if (st == St::Failed) {
		CHECK(p.error() != HttpError::None);
		CHECK(p.feed(reinterpret_cast<const uint8_t*>("x"), 1) == St::Failed);
	}
}

static void testFuzz() {
	std::mt19937 rng(12345);
	const std::string valid[] = {
		"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nContent-Type: text/plain\r\n\r\n0123456789",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-T: v\r\n\r\n",
		"HTTP/1.0 200 OK\r\nServer: x\r\n\r\nread until close",
		"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi",
		"HTTP/1.1 302 Found\r\nLocation: http://a.example/b\r\nContent-Length: 0\r\n\r\n",
		"HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nabc",
	};
	// Pure noise, including noise that starts like a status line.
	for (int i = 0; i < 1500; i++) {
		std::string w(rng() % 300, 0);
		for (char& c : w) c = static_cast<char>(rng());
		if (i % 3 == 0) w = "HTTP/1.1 200 OK\r\n" + w;
		fuzzOne(w, rng, i % 4 == 0 ? 64 : 0);
	}
	// Mutated valid responses: flip, insert and delete bytes, then truncate sometimes.
	for (int i = 0; i < 3000; i++) {
		std::string w = valid[rng() % (sizeof valid / sizeof valid[0])];
		int edits = 1 + rng() % 4;
		for (int e = 0; e < edits && !w.empty(); e++) {
			size_t at = rng() % w.size();
			switch (rng() % 3) {
			case 0: w[at] = static_cast<char>(rng()); break;
			case 1: w.insert(at, 1, static_cast<char>(rng())); break;
			default: w.erase(at, 1); break;
			}
		}
		if (rng() % 5 == 0 && !w.empty()) w.resize(rng() % w.size());
		fuzzOne(w, rng, i % 5 == 0 ? 8 : 0);
	}
	// Unbounded header line and unbounded header count must fail rather than buffer forever.
	{
		MemorySink sink;
		ResponseParser p(sink);
		std::string head = "HTTP/1.1 200 OK\r\nX: ";
		St st = p.feed(reinterpret_cast<const uint8_t*>(head.data()), head.size());
		std::string junk(4096, 'a');
		for (int i = 0; i < 64 && st == St::NeedMore; i++) st = p.feed(reinterpret_cast<const uint8_t*>(junk.data()), junk.size());
		CHECK(st == St::Failed);
	}
	{
		MemorySink sink;
		ResponseParser p(sink);
		std::string head = "HTTP/1.1 200 OK\r\n";
		St st = p.feed(reinterpret_cast<const uint8_t*>(head.data()), head.size());
		std::string h = "X-Header: value\r\n";
		for (int i = 0; i < 20000 && st == St::NeedMore; i++) st = p.feed(reinterpret_cast<const uint8_t*>(h.data()), h.size());
		CHECK(st == St::Failed);
	}
}

static void testDescribe() {
	HttpResult r;
	CHECK(describe(r) == "OK");
	const HttpError all[] = {HttpError::NoNetwork, HttpError::Dns, HttpError::Connect, HttpError::Timeout,
	                         HttpError::HttpStatus, HttpError::TlsRequired, HttpError::TooLarge, HttpError::Parse,
	                         HttpError::Io, HttpError::Cancelled, HttpError::Redirects};
	for (HttpError e : all) {
		r.error = e;
		CHECK(!describe(r).empty() && describe(r) != "Unknown error.");
	}
	r.error = HttpError::HttpStatus;
	r.status = 503;
	CHECK(describe(r).find("503") != std::string::npos);
}

static void testDottedQuad() {
	uint32_t v = 0;
	CHECK(httpparse::parseDottedQuad("1.2.3.4", v) && v == 0x01020304u);
	CHECK(httpparse::parseDottedQuad("0.0.0.0", v) && v == 0);
	CHECK(httpparse::parseDottedQuad("255.255.255.255", v) && v == 0xFFFFFFFFu);
	CHECK(httpparse::parseDottedQuad("127.0.0.1", v) && v == 0x7F000001u);
	// Everything else is a hostname (or garbage) for the resolver; `out` must stay untouched on failure.
	v = 0xDEADBEEFu;
	for (const char* bad : {"256.1.1.1", "1.256.1.1", "1.1.1.256", "1.2.3", "1.2.3.4.5", "", "1.2.3.4.", ".1.2.3.4", "1..2.3",
	                        " 1.2.3.4", "1.2.3.4 ", "1. 2.3.4", "1.2.3.4\n", "-1.2.3.4", "+1.2.3.4", "1.2.3.x", "a.b.c.d",
	                        "localhost", "1234.1.1.1", "0x7f.0.0.1", "1.2.3.4:80"}) {
		CHECK(!httpparse::parseDottedQuad(bad, v));
	}
	// Leading zeros are rejected (inet_aton would read "010" as octal 8): "0" alone is fine, "00" and "01" are not.
	CHECK(!httpparse::parseDottedQuad("01.2.3.4", v));
	CHECK(!httpparse::parseDottedQuad("1.2.3.04", v));
	CHECK(!httpparse::parseDottedQuad("1.00.3.4", v));
	CHECK(!httpparse::parseDottedQuad("001.2.3.4", v));
	CHECK(v == 0xDEADBEEFu);
}

int main() {
	testDottedQuad();
	testUrl();
	testResolve();
	testRequest();
	testContentLength();
	testChunked();
	testUntilClose();
	testTooLarge();
	testRedirectAndStatus();
	testMalformed();
	testSinkRefusal();
	testFileSink();
	testFileSinkReplace();
	testMemorySinkLimits();
	testInterimCap();
	testFuzz();
	testDescribe();
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("http_parse_test: all passed\n");
	return 0;
}
