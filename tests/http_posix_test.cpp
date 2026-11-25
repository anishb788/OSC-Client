// PosixTransport + httpGet against the loopback server (no internet needed).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "LoopbackServer.h"
#include "net/Http.h"
#include "net/HttpPosix.h"
#include "net/HttpTransfer.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static HttpResult get(const std::string& url, MemorySink& sink, HttpRequest req = HttpRequest(), CancelFlag cancel = nullptr) {
	req.url = url;
	PosixTransport tr;
	return httpGet(req, sink, nullptr, cancel, tr);
}

static bool exists(const std::string& p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0;
}

int main() {
	alarm(120);   // a hang kills the run instead of blocking CI
	TempDir tmp;
	LoopbackServer srv;

	{   // Content-Length, bigger than one step's byte budget
		MemorySink sink(0);
		HttpResult r = get(srv.url("/len/1500000"), sink);
		CHECK(r.ok());
		CHECK(r.status == 200);
		CHECK(sink.body() == patternBody(1500000));
	}
	{   // chunked
		MemorySink sink(0);
		HttpResult r = get(srv.url("/chunked/300000/777"), sink);
		CHECK(r.ok());
		CHECK(sink.body() == patternBody(300000));
	}
	{   // redirect is followed, final URL reflects it
		MemorySink sink(0);
		HttpResult r = get(srv.url("/redir/len/1000"), sink);
		CHECK(r.ok());
		CHECK(sink.body() == patternBody(1000));
		CHECK(r.finalUrl == srv.url("/len/1000"));
	}
	{   // redirect to https
		MemorySink sink;
		HttpResult r = get(srv.url("/redir-https"), sink);
		CHECK(r.error == HttpError::TlsRequired);
	}
	{   // stall: Timeout about when the idle timer says
		MemorySink sink;
		HttpRequest req;
		req.idleTimeoutMs = 400;
		int64_t t0 = nowMs();
		HttpResult r = get(srv.url("/stall"), sink, req);
		int64_t dt = nowMs() - t0;
		CHECK(r.error == HttpError::Timeout);
		CHECK(dt >= 350);      // the idle timer is 400 ms
			CHECK(dt < 5000);      // hang detector, not a timing assertion
	}
	{   // 404 and the User-Agent
		MemorySink sink;
		HttpResult r = get(srv.url("/nothing"), sink);
		CHECK(r.error == HttpError::HttpStatus && r.status == 404);
		MemorySink hs;
		CHECK(get(srv.url("/headers"), hs).ok());
		CHECK(hs.body().find("User-Agent: OpenShop/") != std::string::npos);
		CHECK(hs.body().find("Host: 127.0.0.1:") != std::string::npos);
	}
	{   // connection refused: grab a port, close it
		int port;
		{
			LoopbackServer gone;
			port = gone.port();
		}
		MemorySink sink;
		HttpResult r = get("http://127.0.0.1:" + std::to_string(port) + "/x", sink);
		CHECK(r.error == HttpError::Connect);
	}
	{   // server resets: an error, never SIGPIPE
		MemorySink sink;
		HttpResult r = get(srv.url("/reset"), sink);
		CHECK(!r.ok());
		PosixTransport tr;
		CHECK(tr.beginConnect(0x7f000001, srv.port()) == HttpError::None);
		for (int i = 0; i < 100 && tr.checkConnected() == Transport::ConnectState::InProgress; i++) usleep(1000);
		std::string req = "GET /reset HTTP/1.0\r\n\r\n";
		Transport::IoResult w{};
		for (int i = 0; i < 100; i++) {
			w = tr.send(reinterpret_cast<const uint8_t*>(req.data()), req.size());
			if (w.status == Transport::IoStatus::Error) break;
			usleep(5000);
		}
		CHECK(w.status == Transport::IoStatus::Error);   // reaching this line means the process survived
	}
	{   // cancel mid-body from another thread: fast, no .tmp
		std::string path = tmp.file("cancel.bin");
		std::remove(path.c_str());
		std::atomic<bool> cancel{false};
		std::thread canceller([&] {
			usleep(250000);
			cancel = true;
		});
		HttpRequest req;
		req.url = srv.url("/slow/4000000/200000");
		PosixTransport tr;
		int64_t t0 = nowMs();
		{
			FileSink sink(path);
			HttpResult r = httpGet(req, sink, nullptr, &cancel, tr);
			CHECK(r.error == HttpError::Cancelled);
		}
		int64_t dt = nowMs() - t0;
		canceller.join();
		CHECK(dt < 3000);   // hang detector
		CHECK(!exists(path + ".tmp"));
		CHECK(!exists(path));
	}
	{   // a one-byte-at-a-time step budget still produces the identical body
		MemorySink sink(0);
		HttpRequest req;
		req.url = srv.url("/len/20000");
		PosixTransport tr;
		HttpTransfer x(req, sink, nullptr, nullptr, tr);
		StepBudget b;
		b.maxBytes = 1;
		b.maxMs = 1;
		while (x.step(b) == HttpTransfer::StepResult::Pending) {}
		CHECK(x.result().ok());
		CHECK(sink.body() == patternBody(20000));
	}

	{   // a client that connects, asks for far more than the socket buffers hold, never reads and never closes: the
		// server's destructor must still return
		int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
		auto s2 = std::make_unique<LoopbackServer>();
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = htons(static_cast<uint16_t>(s2->port()));
		CHECK(::connect(cfd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
		const char req[] = "GET /len/40000000 HTTP/1.0\r\n\r\n";
		CHECK(::send(cfd, req, sizeof req - 1, 0) == static_cast<ssize_t>(sizeof req - 1));
		for (int i = 0; i < 2500 && s2->requests().empty(); i++) usleep(2000);
		usleep(300000);   // let the server fill the socket buffers and block on the unread data
		int64_t t0 = nowMs();
		s2.reset();
		CHECK(nowMs() - t0 < 3000);   // hang detector
		::close(cfd);
	}

	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("http_posix_test: ok\n");
	return 0;
}
