// PC dev tool: downloads URLs through the real worker, PosixTransport and FileSink while printing progress. The
// streaming path M3's archive downloads use.
//   httpget URL OUT
//   httpget --contend BG_URL BG_OUT DELAY_MS CAT_URL CAT_OUT   (Background first, then a Catalog job after the delay)
// Ctrl+C cancels everything, waits for the worker to stop and leaves no .tmp file.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "net/HttpPosix.h"
#include "net/Worker.h"

using namespace net;

static std::atomic<bool> gInterrupted{false};
static void onSigint(int) { gInterrupted = true; }

static int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Tracked {
	std::string name;
	JobId id = 0;
	int64_t startMs = 0;
	std::atomic<bool> finished{false};
	std::atomic<int> exitCode{1};
};

static void submit(Worker& w, Tracked& t, JobClass cls, const std::string& url, const std::string& out) {
	HttpRequest req;
	req.url = url;
	t.startMs = nowMs();
	Tracked* tp = &t;
	t.id = w.submit(cls, t.name, std::make_unique<HttpGetJob>(req, std::make_unique<FileSink>(out),
		[tp, out](HttpSink& sink, const HttpResult& r, JobContext& ctx) {
			bool ok = r.ok() && static_cast<FileSink&>(sink).commit();
			std::string line = tp->name + ": " + (ok ? "done " + std::to_string(r.bytesReceived) + " bytes -> " + out : "failed: " + describe(r) + " (" + r.detail + ")");
			int64_t ms = nowMs() - tp->startMs;
			tp->exitCode = ok ? 0 : 1;
			ctx.post([tp, line, ms] {
				std::printf("%s in %lld ms\n", line.c_str(), static_cast<long long>(ms));
				tp->finished = true;
			});
		}));
}

int main(int argc, char** argv) {
	bool contend = argc == 7 && std::string(argv[1]) == "--contend";
	if (!contend && argc != 3) {
		std::fprintf(stderr, "usage: httpget URL OUT\n       httpget --contend BG_URL BG_OUT DELAY_MS CAT_URL CAT_OUT\n");
		return 2;
	}
	std::signal(SIGINT, onSigint);

	Worker w([] { return makePosixTransport(); });
	if (!w.start()) return 1;
	Tracked a, b;
	int64_t t0 = nowMs();
	int64_t delay = 0;
	if (contend) {
		a.name = "background";
		b.name = "catalog";
		delay = std::atoll(argv[4]);
		submit(w, a, JobClass::Background, argv[2], argv[3]);
	} else {
		a.name = "download";
		submit(w, a, JobClass::Catalog, argv[1], argv[2]);
	}

	bool bStarted = !contend;
	int64_t lastPrint = 0;
	while (!gInterrupted) {
		w.drainResults();
		int64_t now = nowMs();
		if (!bStarted && now - t0 >= delay) {
			submit(w, b, JobClass::Catalog, argv[5], argv[6]);
			bStarted = true;
		}
		if (now - lastPrint >= 500) {
			lastPrint = now;
			for (Tracked* t : {&a, &b}) {
				uint64_t rec;
				int64_t total;
				if (t->id && !t->finished && w.progress(t->id, rec, total))
					std::printf("%s: %llu / %lld bytes (%lld ms)\n", t->name.c_str(), static_cast<unsigned long long>(rec),
					            static_cast<long long>(total), static_cast<long long>(now - t0));
			}
			std::fflush(stdout);
		}
		if (a.finished && (b.finished || !contend)) break;
		usleep(5000);
	}
	int rc = 0;
	if (gInterrupted) {
		int64_t c0 = nowMs();
		w.stop();
		std::printf("cancelled; worker stopped in %lld ms\n", static_cast<long long>(nowMs() - c0));
		rc = 130;
	} else {
		w.stop();
		rc = a.exitCode | (contend ? b.exitCode.load() : 0);
	}
	return rc;
}
