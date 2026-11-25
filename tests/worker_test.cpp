// The network worker: scheduling, cancel, shutdown and result delivery with scripted jobs, then real transfers over
// loopback sockets. Needs SDL only for threads and mutexes (no SDL_Init).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "LoopbackServer.h"
#include "net/HttpPosix.h"
#include "net/Worker.h"

using namespace net;

static std::atomic<int> failures{0};
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static Worker::TransportFactory posixFactory() { return [] { return makePosixTransport(); }; }

// Waits (up to timeoutMs) until pred() holds, draining results as the main thread would.
template <class F>
static bool waitFor(Worker& w, int timeoutMs, F pred) {
	int64_t end = nowMs() + timeoutMs;
	while (nowMs() < end) {
		w.drainResults();
		if (pred()) return true;
		usleep(2000);
	}
	w.drainResults();
	return pred();
}

static bool exists(const std::string& p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0;
}

// Takes `steps` steps (each sleeping sleepMs) and records its step order in a shared log.
struct Log {
	std::mutex mu;
	std::vector<std::string> events;
	void add(const std::string& s) {
		std::lock_guard<std::mutex> l(mu);
		events.push_back(s);
	}
	std::vector<std::string> get() {
		std::lock_guard<std::mutex> l(mu);
		return events;
	}
};

class CountJob : public Job {
public:
	CountJob(Log& log, std::string name, int steps, int sleepMs = 0, bool endless = false)
		: log_(log), name_(std::move(name)), steps_(steps), sleepMs_(sleepMs), endless_(endless) {}
	JobStep step(JobContext& ctx) override {
		log_.add(name_);
		if (sleepMs_) usleep(static_cast<useconds_t>(sleepMs_ * 1000));
		ctx.setProgress(static_cast<uint64_t>(++n_), steps_);
		if (ctx.cancelled()) {
			log_.add(name_ + ":cancelled");
			return JobStep::Done;
		}
		if (!endless_ && n_ >= steps_) {
			std::string name = name_;
			ctx.post([this_log = &log_, name] { this_log->add(name + ":result"); });
			return JobStep::Done;
		}
		return JobStep::Pending;
	}
private:
	Log& log_;
	std::string name_;
	int steps_, sleepMs_;
	bool endless_;
	int n_ = 0;
};

// Never finishes, whatever happens.
class StubbornJob : public Job {
public:
	explicit StubbornJob(std::atomic<int>& steps) : steps_(steps) {}
	JobStep step(JobContext&) override {
		steps_++;
		usleep(1000);
		return JobStep::Pending;
	}
private:
	std::atomic<int>& steps_;
};

static size_t count(const std::vector<std::string>& v, const std::string& s) {
	size_t n = 0;
	for (const auto& e : v) n += e == s;
	return n;
}

static void testLifecycle() {
	{
		Worker w(nullptr);
		CHECK(w.start());
		CHECK(w.running());
		CHECK(!w.start());
		w.stop();
		CHECK(!w.running());
		w.stop();
		std::atomic<int> unused{0};
		CHECK(w.submit(JobClass::Catalog, "x", std::make_unique<StubbornJob>(unused)) == 0);   // rejected, job freed
	}
	{
		Worker w(nullptr);   // destructor alone stops it
		CHECK(w.start());
	}
	{
		Worker w(nullptr);   // never started: queued jobs are just destroyed
		Log log;
		CHECK(w.submit(JobClass::Icon, "a", std::make_unique<CountJob>(log, "a", 1)) != 0);
		CHECK(w.pending() == 1);
	}
}

static void testResultsOnCallerThread() {
	Worker w(nullptr);
	w.start();
	Log log;
	std::thread::id mainId = std::this_thread::get_id();
	std::atomic<int> wrongThread{0};
	std::atomic<int> ran{0};
	for (int i = 0; i < 5; i++) {
		w.submit(JobClass::Icon, "i", std::make_unique<CountJob>(log, "j" + std::to_string(i), 2));
		w.post([&] {
			if (std::this_thread::get_id() != mainId) wrongThread++;
			ran++;
		});
	}
	CHECK(waitFor(w, 3000, [&] { return ran == 5 && w.pending() == 0 && count(log.get(), "j4:result") == 1; }));
	CHECK(wrongThread == 0);
	for (int i = 0; i < 5; i++) CHECK(count(log.get(), "j" + std::to_string(i) + ":result") == 1);
}

static void testPriority() {
	// All three classes active at once: the catalog job's steps all come before the background job's, and the icon
	// job sits in between.
	Worker w(nullptr);
	Log log;
	// Submitted before start so all are active on the first pick.
	w.submit(JobClass::Background, "b", std::make_unique<CountJob>(log, "B", 10, 1));
	w.submit(JobClass::Icon, "i", std::make_unique<CountJob>(log, "I", 10, 1));
	w.submit(JobClass::Catalog, "c", std::make_unique<CountJob>(log, "C", 10, 1));
	w.start();
	CHECK(waitFor(w, 3000, [&] { return w.pending() == 0; }));
	std::string order;
	for (auto& e : log.get())
		if (e.size() == 1) order += e;
	CHECK(order == "CCCCCCCCCCIIIIIIIIIIBBBBBBBBBB");
}

static void testBackgroundStarvation() {
	Worker w(nullptr);
	w.start();
	Log log;
	w.submit(JobClass::Background, "b", std::make_unique<CountJob>(log, "B", 1000000, 0, true));
	w.submit(JobClass::Catalog, "c", std::make_unique<CountJob>(log, "C", 1000000, 5, true));
	usleep(1200000);
	auto ev = log.get();
	w.cancelAll();
	CHECK(waitFor(w, 2000, [&] { return w.pending() == 0; }));
	// ~1.2 s with a slice due every 250 ms: 3 to 5 slices, never zero and never a flood.
	size_t b = count(ev, "B");
	CHECK(b >= 3 && b <= 6);
	CHECK(count(ev, "C") > 100);
}

static void testIconNewestFirstAndDrop() {
	Worker w(nullptr);
	w.start();
	Log log;
	std::atomic<bool> release{false};
	// The first icon job occupies the single Icon slot until released.
	struct Gate : Job {
		Log& log;
		std::atomic<bool>& release;
		Gate(Log& l, std::atomic<bool>& r) : log(l), release(r) {}
		JobStep step(JobContext& ctx) override {
			if (release || ctx.cancelled()) return JobStep::Done;
			usleep(1000);
			return JobStep::Pending;
		}
	};
	w.submit(JobClass::Icon, "gate", std::make_unique<Gate>(log, release));
	usleep(20000);
	w.submit(JobClass::Icon, "old", std::make_unique<CountJob>(log, "old", 1));
	w.submit(JobClass::Icon, "mid", std::make_unique<CountJob>(log, "mid", 1));
	w.submit(JobClass::Icon, "new", std::make_unique<CountJob>(log, "new", 1));
	w.submit(JobClass::Icon, "gone", std::make_unique<CountJob>(log, "gone", 1));
	size_t dropped = w.dropWhere([](const JobInfo& j) { return j.tag == "gone"; });
	CHECK(dropped == 1);
	CHECK(waitFor(w, 1000, [&] { return count(log.get(), "gone:cancelled") == 1; }));   // finished through its own path
	release = true;
	CHECK(waitFor(w, 3000, [&] { return w.pending() == 0; }));
	std::vector<std::string> order;
	for (auto& e : log.get())
		if (e == "new" || e == "mid" || e == "old") order.push_back(e);
	CHECK(order.size() == 3 && order[0] == "new" && order[1] == "mid" && order[2] == "old");
	CHECK(count(log.get(), "gone:result") == 0);
}

static void testProgressAndCancelById() {
	Worker w(nullptr);
	w.start();
	Log log;
	JobId id = w.submit(JobClass::Catalog, "p", std::make_unique<CountJob>(log, "P", 0, 2, true));
	CHECK(id != 0);
	usleep(100000);
	uint64_t rec = 0;
	int64_t total = 0;
	CHECK(w.progress(id, rec, total));
	CHECK(rec > 5);
	CHECK(w.cancel(id));
	CHECK(waitFor(w, 1000, [&] { return w.pending() == 0; }));
	CHECK(!w.progress(id, rec, total));
	CHECK(!w.cancel(id));
}

static void testStopIsFast() {
	Worker w(nullptr);
	w.start();
	Log log;
	std::atomic<int> stubborn{0};
	for (int i = 0; i < 6; i++) w.submit(JobClass::Background, "b", std::make_unique<CountJob>(log, "B" + std::to_string(i), 0, 20, true));
	w.submit(JobClass::Catalog, "c", std::make_unique<CountJob>(log, "C", 0, 20, true));
	w.submit(JobClass::Icon, "s", std::make_unique<StubbornJob>(stubborn));
	usleep(100000);
	int64_t t0 = nowMs();
	w.stop();
	int64_t dt = nowMs() - t0;
	CHECK(dt < 500);
	CHECK(stubborn.load() < 100);   // the one that ignores cancel is stepped at most kStopSteps more times, then dropped
	auto ev = log.get();
	CHECK(count(ev, "C:cancelled") == 1);
	CHECK(count(ev, "B0:cancelled") == 1);
	CHECK(count(ev, "B5:cancelled") == 1);   // a queued job also finishes through its own path
}

static void testHttpJobs() {
	LoopbackServer srv;
	Worker w(posixFactory());
	w.start();

	// A byte-exact body through HttpGetJob, result delivered on the main thread.
	std::atomic<int> got{0};
	std::string body;
	HttpError err = HttpError::Io;
	HttpRequest req;
	req.url = srv.url("/len/470000");
	w.submit(JobClass::Catalog, "cat", std::make_unique<HttpGetJob>(req, std::make_unique<MemorySink>(0),
		[&](HttpSink& sink, const HttpResult& r, JobContext& ctx) {
			std::string b = static_cast<MemorySink&>(sink).body();
			HttpError e = r.error;
			ctx.post([&, b, e] {
				body = b;
				err = e;
				got++;
			});
		}));
	CHECK(waitFor(w, 5000, [&] { return got == 1; }));
	CHECK(err == HttpError::None);
	CHECK(body == patternBody(470000));
	CHECK(srv.requests().size() == 1);

	// Contention: a slow Background download (1 MB at 400 KB/s) with a Catalog job started halfway through.
	const uint64_t kBg = 1000000;
	std::string bgPath = "/tmp/openshop_worker_test_bg.bin";
	std::remove(bgPath.c_str());
	std::atomic<int64_t> bgDone{0}, catDone{0};
	std::atomic<bool> bgOk{false};
	HttpRequest bgReq;
	bgReq.url = srv.url("/slow/" + std::to_string(kBg) + "/400000");
	JobId bgId = w.submit(JobClass::Background, "bg", std::make_unique<HttpGetJob>(bgReq, std::make_unique<FileSink>(bgPath),
		[&](HttpSink& sink, const HttpResult& r, JobContext&) {
			bgOk = r.ok() && static_cast<FileSink&>(sink).commit();
			bgDone = nowMs();
		}));
	usleep(1000000);
	uint64_t recAtStart = 0;
	int64_t total = 0;
	CHECK(w.progress(bgId, recAtStart, total));
	CHECK(recAtStart > 0 && recAtStart < kBg);
	HttpRequest catReq;
	catReq.url = srv.url("/len/470000");
	MemorySink* catSink = new MemorySink(0);
	std::string catBody;
	int64_t t0 = nowMs();
	w.submit(JobClass::Catalog, "cat2", std::make_unique<HttpGetJob>(catReq, std::unique_ptr<HttpSink>(catSink),
		[&](HttpSink& sink, const HttpResult&, JobContext&) {
			catBody = static_cast<MemorySink&>(sink).body();
			catDone = nowMs();
		}));
	CHECK(waitFor(w, 5000, [&] { return catDone != 0 && bgDone != 0; }));
	CHECK(catBody == patternBody(470000));
	CHECK(catDone - t0 < 400);
	CHECK(catDone < bgDone);
	CHECK(bgOk);
	FILE* f = std::fopen(bgPath.c_str(), "rb");
	std::string file;
	if (f) {
		char b[8192];
		size_t n;
		while ((n = std::fread(b, 1, sizeof b, f)) > 0) file.append(b, n);
		std::fclose(f);
	}
	CHECK(file == patternBody(kBg));
	std::remove(bgPath.c_str());
}

static void testHttpCancelAndStop() {
	LoopbackServer srv;
	Worker w(posixFactory());
	w.start();
	std::string p1 = "/tmp/openshop_worker_test_c1.bin", p2 = "/tmp/openshop_worker_test_c2.bin";
	for (auto& p : {p1, p2}) std::remove(p.c_str());
	HttpRequest r1, r2;
	r1.url = srv.url("/slow/4000000/200000");
	r2.url = srv.url("/slow/4000000/200000");
	w.submit(JobClass::Background, "a", std::make_unique<HttpGetJob>(r1, std::make_unique<FileSink>(p1)));
	w.submit(JobClass::Catalog, "b", std::make_unique<HttpGetJob>(r2, std::make_unique<FileSink>(p2)));
	usleep(300000);
	CHECK(exists(p1 + ".tmp") && exists(p2 + ".tmp"));
	int64_t t0 = nowMs();
	w.stop();
	CHECK(nowMs() - t0 < 1000);
	for (auto& p : {p1, p2}) CHECK(!exists(p + ".tmp") && !exists(p));

	// A stalled server does not delay stop either (and the idle timeout is far away).
	Worker w2(posixFactory());
	w2.start();
	HttpRequest st;
	st.url = srv.url("/stall");
	w2.submit(JobClass::Catalog, "s", std::make_unique<HttpGetJob>(st, std::make_unique<MemorySink>()));
	usleep(200000);
	t0 = nowMs();
	w2.stop();
	CHECK(nowMs() - t0 < 500);
}

int main() {
	testLifecycle();
	testResultsOnCallerThread();
	testPriority();
	testBackgroundStarvation();
	testIconNewestFirstAndDrop();
	testProgressAndCancelById();
	testStopIsFast();
	testHttpJobs();
	testHttpCancelAndStop();
	if (failures) {
		std::printf("%d failure(s)\n", failures.load());
		return 1;
	}
	std::printf("worker_test: ok\n");
	return 0;
}
