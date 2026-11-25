// The network worker: scheduling, cancel, shutdown and result delivery with scripted jobs, then real transfers over
// loopback sockets. Needs SDL only for threads and mutexes (no SDL_Init).
//
// Timing rules: every Worker here runs at normal thread priority (the LOW priority starves under CPU load), waits are
// conditions polled against a generous deadline, and wall-clock bounds are only hang detectors. State that jobs and
// closures capture by reference is declared before the Worker, so it outlives the stop() in the destructor.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
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

// Takes `steps` steps (each sleeping sleepMs) and records its step order in a shared log. With a gate it idles (no
// log entry, no progress) until the gate opens, so a test can have several jobs active before any of them runs.
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
	CountJob(Log& log, std::string name, int steps, int sleepMs = 0, bool endless = false, std::atomic<bool>* gate = nullptr)
		: log_(log), name_(std::move(name)), steps_(steps), sleepMs_(sleepMs), endless_(endless), gate_(gate) {}
	JobStep step(JobContext& ctx) override {
		if (gate_ && !*gate_ && !ctx.cancelled()) {
			usleep(500);
			return JobStep::Pending;
		}
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
	std::atomic<bool>* gate_;
	int n_ = 0;
};

// Never finishes, whatever happens. `cancelledSteps` counts the steps that saw the cancel flag.
class StubbornJob : public Job {
public:
	StubbornJob(std::atomic<int>& steps, std::atomic<int>& cancelledSteps, int sleepUs = 1000)
		: steps_(steps), cancelledSteps_(cancelledSteps), sleepUs_(sleepUs) {}
	JobStep step(JobContext& ctx) override {
		steps_++;
		if (ctx.cancelled()) cancelledSteps_++;
		usleep(static_cast<useconds_t>(sleepUs_));
		return JobStep::Pending;
	}
private:
	std::atomic<int>& steps_;
	std::atomic<int>& cancelledSteps_;
	int sleepUs_;
};

// Returns Pending at once, never waits, honours cancel: what a buggy job that polls in a loop looks like.
class SpinJob : public Job {
public:
	explicit SpinJob(std::atomic<int>& steps) : steps_(steps) {}
	JobStep step(JobContext& ctx) override {
		steps_++;
		return ctx.cancelled() ? JobStep::Done : JobStep::Pending;
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
	Log log;
	std::atomic<int> steps{0}, cancelledSteps{0};
	{
		Worker w(nullptr, false);
		CHECK(!w.running());
		// Not started: nothing would ever run it, so it is refused (and freed), not queued.
		CHECK(w.submit(JobClass::Icon, "a", std::make_unique<CountJob>(log, "a", 1)) == 0);
		CHECK(w.pending() == 0);
		CHECK(w.start());
		CHECK(w.running());
		CHECK(!w.start());
		CHECK(w.submit(JobClass::Icon, "b", std::make_unique<CountJob>(log, "b", 1)) != 0);
		CHECK(waitFor(w, 5000, [&] { return count(log.get(), "b:result") == 1; }));
		w.stop();
		CHECK(!w.running());
		w.stop();
		CHECK(w.submit(JobClass::Catalog, "x", std::make_unique<StubbornJob>(steps, cancelledSteps)) == 0);   // rejected, job freed
		CHECK(w.pending() == 0);
		CHECK(!w.start());   // cannot be restarted
	}
	{
		Worker w(nullptr);   // default (LOW) priority; destructor alone stops it
		CHECK(w.start());
	}
}

static void testResultsOnCallerThread() {
	Log log;
	std::thread::id mainId = std::this_thread::get_id();
	std::atomic<int> wrongThread{0};
	std::atomic<int> ran{0};
	Worker w(nullptr, false);
	w.start();
	for (int i = 0; i < 5; i++) {
		w.submit(JobClass::Icon, "i", std::make_unique<CountJob>(log, "j" + std::to_string(i), 2));
		w.post([&] {
			if (std::this_thread::get_id() != mainId) wrongThread++;
			ran++;
		});
	}
	CHECK(waitFor(w, 5000, [&] { return ran == 5 && w.pending() == 0 && count(log.get(), "j4:result") == 1; }));
	CHECK(wrongThread == 0);
	for (int i = 0; i < 5; i++) CHECK(count(log.get(), "j" + std::to_string(i) + ":result") == 1);
}

static void testThrowingClosure() {
	std::atomic<int> ran{0};
	Worker w(nullptr, false);
	w.start();
	w.post([&] { ran++; });
	w.post([&] { throw 1; });
	w.post([&] { throw std::runtime_error("boom"); });
	w.post([&] { ran++; });
	CHECK(w.drainResults() == 4);
	CHECK(ran == 2);   // the closures after a throwing one still ran
}

static void testPriority() {
	// All three classes active at once (a gate holds every job back until all are submitted). Classes never overlap
	// except through the Background starvation slice, so only relative order is asserted.
	Log log;
	std::atomic<bool> go{false};
	Worker w(nullptr, false);
	w.start();
	w.submit(JobClass::Background, "b", std::make_unique<CountJob>(log, "B", 10, 1, false, &go));
	w.submit(JobClass::Icon, "i", std::make_unique<CountJob>(log, "I", 10, 1, false, &go));
	w.submit(JobClass::Catalog, "c", std::make_unique<CountJob>(log, "C", 10, 1, false, &go));
	go = true;
	CHECK(waitFor(w, 10000, [&] { return w.pending() == 0; }));
	int lastC = -1, firstI = -1, lastI = -1, lastB = -1, nC = 0, nI = 0, nB = 0;
	int idx = 0;
	for (auto& e : log.get()) {
		if (e.size() == 1) {
			if (e == "C") { lastC = idx; nC++; }
			if (e == "I") { if (firstI < 0) firstI = idx; lastI = idx; nI++; }
			if (e == "B") { lastB = idx; nB++; }
		}
		idx++;
	}
	CHECK(nC == 10 && nI == 10 && nB == 10);
	CHECK(firstI > lastC);   // an Icon job never runs while a Catalog job is active
	CHECK(lastB > lastI);    // the Background job finishes last (at most a starvation slice or two ran early)
}

static void testBackgroundStarvation() {
	Log log;
	Worker w(nullptr, false);
	w.start();
	// Catalog first so the Background job is already "starved" from its activation on.
	w.submit(JobClass::Catalog, "c", std::make_unique<CountJob>(log, "C", 1000000, 5, true));
	w.submit(JobClass::Background, "b", std::make_unique<CountJob>(log, "B", 1000000, 0, true));
	// The Background job must get slices while Catalog work is busy (about one per 250 ms), but never a flood.
	CHECK(waitFor(w, 10000, [&] { return count(log.get(), "B") >= 2; }));
	auto ev = log.get();
	w.cancelAll();
	CHECK(waitFor(w, 5000, [&] { return w.pending() == 0; }));
	size_t b = count(ev, "B"), c = count(ev, "C");
	CHECK(b >= 2);
	CHECK(c >= 4 * b);   // the Catalog job kept the thread (about 50 steps per Background slice)
}

static void testIconNewestFirstAndDrop() {
	Log log;
	std::atomic<bool> release{false}, started{false};
	// The first icon job occupies the single Icon slot until released.
	struct Gate : Job {
		std::atomic<bool>& release;
		std::atomic<bool>& started;
		Gate(std::atomic<bool>& r, std::atomic<bool>& s) : release(r), started(s) {}
		JobStep step(JobContext& ctx) override {
			started = true;
			if (release || ctx.cancelled()) return JobStep::Done;
			usleep(1000);
			return JobStep::Pending;
		}
	};
	Worker w(nullptr, false);
	w.start();
	w.submit(JobClass::Icon, "gate", std::make_unique<Gate>(release, started));
	CHECK(waitFor(w, 5000, [&] { return started.load(); }));
	w.submit(JobClass::Icon, "old", std::make_unique<CountJob>(log, "old", 1));
	w.submit(JobClass::Icon, "mid", std::make_unique<CountJob>(log, "mid", 1));
	w.submit(JobClass::Icon, "new", std::make_unique<CountJob>(log, "new", 1));
	w.submit(JobClass::Icon, "gone", std::make_unique<CountJob>(log, "gone", 1));
	size_t dropped = w.dropWhere([](const JobInfo& j) { return j.tag == "gone"; });
	CHECK(dropped == 1);
	CHECK(w.dropWhere([](const JobInfo& j) { return j.tag == "gone"; }) == 0);   // already cancelled: not counted again
	CHECK(waitFor(w, 5000, [&] { return count(log.get(), "gone:cancelled") == 1; }));   // finished through its own path
	release = true;
	CHECK(waitFor(w, 5000, [&] { return w.pending() == 0; }));
	std::vector<std::string> order;
	for (auto& e : log.get())
		if (e == "new" || e == "mid" || e == "old") order.push_back(e);
	CHECK(order.size() == 3 && order[0] == "new" && order[1] == "mid" && order[2] == "old");
	CHECK(count(log.get(), "gone:result") == 0);
}

static void testProgressAndCancelById() {
	Log log;
	Worker w(nullptr, false);
	w.start();
	JobId id = w.submit(JobClass::Catalog, "p", std::make_unique<CountJob>(log, "P", 0, 2, true));
	CHECK(id != 0);
	uint64_t rec = 0;
	int64_t total = 0;
	CHECK(waitFor(w, 5000, [&] { return w.progress(id, rec, total) && rec > 5; }));
	CHECK(total == 0);
	CHECK(w.cancel(id));
	CHECK(waitFor(w, 5000, [&] { return w.pending() == 0; }));
	CHECK(!w.progress(id, rec, total));
	CHECK(!w.cancel(id));
}

static void testProgressSaturation() {
	JobProgress p;
	CHECK(p.total.load() == JobProgress::kUnknown);
	p.set(5, -1);
	CHECK(p.received.load() == 5 && p.total.load() == JobProgress::kUnknown);
	p.set(0x1FFFFFFFFull, 0x1FFFFFFFFll);   // beyond 4 GB: both saturate, and a total never reads as "unknown"
	CHECK(p.received.load() == 0xFFFFFFFFu);
	CHECK(p.total.load() == 0xFFFFFFFEu);
	p.set(10, 3000000000ll);   // above INT32_MAX, still exact
	CHECK(p.total.load() == 3000000000u);
}

static void testCancelledJobIsDropped() {
	// A job that ignores cancel and always returns Pending would hold the thread forever (a cancelled job is stepped
	// before every other). After kStopSteps steps it is destroyed, and other work proceeds.
	Log log;
	std::atomic<int> steps{0}, cancelledSteps{0};
	Worker w(nullptr, false);
	w.start();
	JobId id = w.submit(JobClass::Catalog, "stuck", std::make_unique<StubbornJob>(steps, cancelledSteps));
	CHECK(waitFor(w, 5000, [&] { return steps > 0; }));
	CHECK(w.cancel(id));
	w.submit(JobClass::Catalog, "after", std::make_unique<CountJob>(log, "after", 3));
	CHECK(waitFor(w, 5000, [&] { return count(log.get(), "after:result") == 1 && w.pending() == 0; }));
	CHECK(cancelledSteps <= Worker::kStopSteps + 1);   // +1: the flag can rise between the worker's check and the job's

	// cancel() of a job that is already cancelled but still present is true; dropWhere does not count it. Each step
	// takes 50 ms, so it stays around for about 400 ms, far longer than the calls below need.
	std::atomic<int> slowSteps{0}, slowCancelled{0};
	JobId slow = w.submit(JobClass::Icon, "slow", std::make_unique<StubbornJob>(slowSteps, slowCancelled, 50000));
	CHECK(waitFor(w, 5000, [&] { return slowSteps > 0; }));
	CHECK(w.cancel(slow));
	CHECK(w.cancel(slow));
	CHECK(w.dropWhere([](const JobInfo&) { return true; }) == 0);
	CHECK(waitFor(w, 5000, [&] { return w.pending() == 0; }));
	CHECK(!w.cancel(slow));
}

static void testNoBusySpin() {
	// A job that returns Pending without waiting is slowed to about 1000 steps/s instead of spinning the CPU.
	std::atomic<int> steps{0};
	Worker w(nullptr, false);
	w.start();
	w.submit(JobClass::Catalog, "spin", std::make_unique<SpinJob>(steps));
	usleep(1000000);   // an upper bound only, so CPU load can only help
	int n = steps.load();
	w.cancelAll();
	CHECK(waitFor(w, 5000, [&] { return w.pending() == 0; }));
	CHECK(n > 0);
	CHECK(n < 2000);
}

static void testStopIsFast() {
	Log log;
	std::atomic<int> stubborn{0}, stubbornCancelled{0};
	Worker w(nullptr, false);
	w.start();
	for (int i = 0; i < 6; i++) w.submit(JobClass::Background, "b", std::make_unique<CountJob>(log, "B" + std::to_string(i), 0, 20, true));
	w.submit(JobClass::Catalog, "c", std::make_unique<CountJob>(log, "C", 0, 20, true));
	w.submit(JobClass::Icon, "s", std::make_unique<StubbornJob>(stubborn, stubbornCancelled));
	CHECK(waitFor(w, 5000, [&] { return count(log.get(), "C") > 0 && count(log.get(), "B0") > 0; }));
	int64_t t0 = nowMs();
	w.stop();
	CHECK(nowMs() - t0 < 3000);   // hang detector
	CHECK(stubbornCancelled <= Worker::kStopSteps + 1);   // the job that ignores cancel is stepped a bounded number of times, then dropped
	auto ev = log.get();
	CHECK(count(ev, "C:cancelled") == 1);
	CHECK(count(ev, "B0:cancelled") == 1);
	CHECK(count(ev, "B5:cancelled") == 1);   // a queued job also finishes through its own path
}

static void testHttpJobs() {
	TempDir tmp;
	LoopbackServer srv;
	// Everything the jobs' callbacks and posted closures capture comes before the Worker.
	std::atomic<int> got{0};
	std::string body;
	HttpError err = HttpError::Io;
	const uint64_t kBg = 1000000;
	std::string bgPath = tmp.file("bg.bin");
	std::atomic<int64_t> bgDone{0}, catDone{0};
	std::atomic<bool> bgOk{false};
	std::string catBody;
	Worker w(posixFactory(), false);
	w.start();

	// A byte-exact body through HttpGetJob, result delivered on the main thread.
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
	CHECK(waitFor(w, 10000, [&] { return got == 1; }));
	CHECK(err == HttpError::None);
	CHECK(body == patternBody(470000));
	CHECK(srv.requests().size() == 1);

	// Contention: a slow Background download (1 MB at 400 KB/s) with a Catalog job started once it is under way. The
	// Catalog job must finish first, i.e. while the download is still incomplete.
	HttpRequest bgReq;
	bgReq.url = srv.url("/slow/" + std::to_string(kBg) + "/400000");
	JobId bgId = w.submit(JobClass::Background, "bg", std::make_unique<HttpGetJob>(bgReq, std::make_unique<FileSink>(bgPath),
		[&](HttpSink& sink, const HttpResult& r, JobContext&) {
			bgOk = r.ok() && static_cast<FileSink&>(sink).commit();
			bgDone = nowMs();
		}));
	uint64_t recAtStart = 0;
	int64_t total = 0;
	CHECK(waitFor(w, 10000, [&] { return w.progress(bgId, recAtStart, total) && recAtStart > 0; }));
	CHECK(recAtStart < kBg);
	HttpRequest catReq;
	catReq.url = srv.url("/len/470000");
	w.submit(JobClass::Catalog, "cat2", std::make_unique<HttpGetJob>(catReq, std::make_unique<MemorySink>(0),
		[&](HttpSink& sink, const HttpResult&, JobContext&) {
			catBody = static_cast<MemorySink&>(sink).body();
			catDone = nowMs();
		}));
	CHECK(waitFor(w, 20000, [&] { return catDone != 0 && bgDone != 0; }));
	CHECK(catBody == patternBody(470000));
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
}

static void testHttpCancelAndStop() {
	TempDir tmp;
	LoopbackServer srv;
	std::string p1 = tmp.file("c1.bin"), p2 = tmp.file("c2.bin");
	Worker w(posixFactory(), false);
	w.start();
	HttpRequest r1, r2;
	r1.url = srv.url("/slow/4000000/200000");
	r2.url = srv.url("/slow/4000000/200000");
	w.submit(JobClass::Background, "a", std::make_unique<HttpGetJob>(r1, std::make_unique<FileSink>(p1)));
	w.submit(JobClass::Catalog, "b", std::make_unique<HttpGetJob>(r2, std::make_unique<FileSink>(p2)));
	CHECK(waitFor(w, 10000, [&] { return exists(p1 + ".tmp") && exists(p2 + ".tmp"); }));
	int64_t t0 = nowMs();
	w.stop();
	CHECK(nowMs() - t0 < 3000);   // hang detector
	for (auto& p : {p1, p2}) CHECK(!exists(p + ".tmp") && !exists(p));

	// A stalled server does not hold up stop either (and the idle timeout is far away).
	Worker w2(posixFactory(), false);
	w2.start();
	HttpRequest st;
	st.url = srv.url("/stall");
	w2.submit(JobClass::Catalog, "s", std::make_unique<HttpGetJob>(st, std::make_unique<MemorySink>()));
	CHECK(waitFor(w2, 10000, [&] { return srv.requests().size() >= 3; }));
	t0 = nowMs();
	w2.stop();
	CHECK(nowMs() - t0 < 3000);   // hang detector
}

int main() {
	alarm(120);   // a hang kills the run instead of blocking CI
	testLifecycle();
	testResultsOnCallerThread();
	testThrowingClosure();
	testPriority();
	testBackgroundStarvation();
	testIconNewestFirstAndDrop();
	testProgressAndCancelById();
	testProgressSaturation();
	testCancelledJobIsDropped();
	testNoBusySpin();
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
