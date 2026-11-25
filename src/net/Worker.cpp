#include "net/Worker.h"

#include <algorithm>
#include <exception>

#include <SDL_mutex.h>
#include <SDL_thread.h>
#include <SDL_timer.h>
#include <SDL_version.h>

#include "core/Log.h"
#include "net/HttpPosix.h"
#include "net/HttpWii.h"

namespace net {

namespace {

// Room for the 8 KB receive buffer, the parser and, later, jansson and a TLS stack. SDL_CreateThread's default is
// whatever the platform picks (a small LWP stack on the Wii is the worry), so ask for a known size.
constexpr size_t kStackBytes = 256 * 1024;

constexpr int kActiveLimit[3] = {1, 1, 2};   // indexed by JobClass

struct Lock {
	explicit Lock(SDL_mutex* m) : m_(m) { SDL_LockMutex(m_); }
	~Lock() { SDL_UnlockMutex(m_); }
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
	SDL_mutex* m_;
};

// Stands in when the factory is empty or returns null: every request fails as NoNetwork.
class NoNetworkTransport : public Transport {
public:
	uint64_t nowMs() override { return SDL_GetTicks(); }
	HttpError resolve(const std::string&, uint32_t&) override { return HttpError::NoNetwork; }
	HttpError beginConnect(uint32_t, int) override { return HttpError::NoNetwork; }
	ConnectState checkConnected() override { return ConnectState::Failed; }
	IoResult send(const uint8_t*, size_t) override { return {IoStatus::Error, 0}; }
	IoResult recv(uint8_t*, size_t) override { return {IoStatus::Error, 0}; }
	void wait(bool, int) override {}
	void close() override {}
};

}  // namespace

Worker::TransportFactory defaultTransportFactory() {
#ifndef __wii__
	return [] { return makePosixTransport(); };
#else
	return [] { return makeWiiTransport(); };
#endif
}

void JobContext::post(std::function<void()> fn) { worker.post(std::move(fn)); }

HttpGetJob::HttpGetJob(HttpRequest req, std::unique_ptr<HttpSink> sink, DoneFn onDone)
	: req_(std::move(req)), sink_(std::move(sink)), onDone_(std::move(onDone)) {}

HttpGetJob::~HttpGetJob() = default;

JobStep HttpGetJob::step(JobContext& ctx) {
	if (!xfer_) {
		JobProgress* prog = &ctx.progress;
		xfer_ = std::make_unique<HttpTransfer>(
			req_, *sink_,
			[prog](uint64_t received, int64_t total) { prog->set(received, total); },
			ctx.cancel, ctx.transport, &ctx.hosts);
	}
	if (xfer_->step(ctx.budget) == HttpTransfer::StepResult::Pending) return JobStep::Pending;
	if (onDone_) onDone_(*sink_, xfer_->result(), ctx);
	return JobStep::Done;
}

// Declaration order matters: members are destroyed in reverse, so the job goes first and the transport it used last.
struct Worker::Entry {
	JobId id = 0;
	JobClass cls = JobClass::Background;
	uint32_t seq = 0;
	std::string tag;
	std::atomic<bool> cancel{false};
	JobProgress progress;
	bool active = false;        // under the Worker lock
	uint32_t lastRun = 0;       // SDL ticks; under the Worker lock
	int cancelSteps = 0;        // steps taken since the cancel flag was seen; worker thread only
	std::unique_ptr<Transport> transport;   // created and destroyed on the worker thread; assigned under the lock
	std::unique_ptr<Job> job;
};

Worker::Worker(TransportFactory factory, bool lowPriority) : factory_(std::move(factory)), lowPriority_(lowPriority) {
	mutex_ = SDL_CreateMutex();
	cond_ = SDL_CreateCond();
}

Worker::~Worker() {
	stop();
	entries_.clear();
	results_.clear();
	SDL_DestroyCond(cond_);
	SDL_DestroyMutex(mutex_);
}

bool Worker::start() {
	if (!mutex_ || !cond_) return false;
	Lock l(mutex_);
	if (thread_ || stopping_) return false;
#if SDL_VERSION_ATLEAST(2, 0, 9)
	thread_ = SDL_CreateThreadWithStackSize(&Worker::threadMain, "net-worker", kStackBytes, this);
#else
	thread_ = SDL_CreateThread(&Worker::threadMain, "net-worker", this);
#endif
	if (!thread_) {
		LOG("net worker: SDL_CreateThread failed: %s", SDL_GetError());
		return false;
	}
	running_ = true;
	return true;
}

void Worker::stop() {
	SDL_Thread* t = nullptr;
	{
		Lock l(mutex_);
		stopping_ = true;
		for (auto& e : entries_) {
			e->cancel.store(true, std::memory_order_relaxed);
			// Lets a backend stuck in an uninterruptible call (libogc DNS, net init) give up so the join below
			// returns. Safe under the lock: an entry leaves entries_ (under it) before its transport is destroyed.
			if (e->transport) e->transport->abort();
		}
		t = thread_;
		thread_ = nullptr;
		SDL_CondBroadcast(cond_);
	}
	if (t) SDL_WaitThread(t, nullptr);
	running_ = false;
}

JobId Worker::submit(JobClass cls, std::string tag, std::unique_ptr<Job> job) {
	if (!job) return 0;
	auto e = std::make_shared<Entry>();
	e->cls = cls;
	e->tag = std::move(tag);
	e->job = std::move(job);
	{
		Lock l(mutex_);
		if (stopping_ || !thread_) return 0;
		e->id = nextId_++;
		e->seq = nextSeq_++;
		entries_.push_back(e);
		SDL_CondBroadcast(cond_);
		return e->id;
	}
}

bool Worker::cancel(JobId id) {
	Lock l(mutex_);
	for (auto& e : entries_) {
		if (e->id == id) {
			e->cancel.store(true, std::memory_order_relaxed);
			return true;
		}
	}
	return false;
}

void Worker::cancelAll() {
	Lock l(mutex_);
	for (auto& e : entries_) e->cancel.store(true, std::memory_order_relaxed);
}

size_t Worker::dropWhere(const std::function<bool(const JobInfo&)>& pred) {
	Lock l(mutex_);
	size_t n = 0;
	for (auto& e : entries_) {
		if (e->cancel.load(std::memory_order_relaxed)) continue;
		if (pred(JobInfo{e->id, e->cls, e->tag, e->active})) {
			e->cancel.store(true, std::memory_order_relaxed);
			n++;
		}
	}
	if (n) SDL_CondBroadcast(cond_);
	return n;
}

bool Worker::progress(JobId id, uint64_t& received, int64_t& total) const {
	Lock l(mutex_);
	for (const auto& e : entries_) {
		if (e->id == id) {
			received = e->progress.received.load(std::memory_order_relaxed);
			uint32_t t = e->progress.total.load(std::memory_order_relaxed);
			total = t == JobProgress::kUnknown ? -1 : static_cast<int64_t>(t);
			return true;
		}
	}
	return false;
}

size_t Worker::pending() const {
	Lock l(mutex_);
	return entries_.size();
}

void Worker::post(std::function<void()> fn) {
	Lock l(mutex_);
	results_.push_back(std::move(fn));
}

size_t Worker::drainResults() {
	std::vector<std::function<void()>> batch;
	{
		Lock l(mutex_);
		batch.swap(results_);
	}
	// Exceptions are enabled in both builds (no -fno-exceptions), so one bad closure must not drop the rest.
	for (auto& fn : batch) {
		try {
			fn();
		} catch (const std::exception& ex) {
			LOG("net worker: a result closure threw: %s", ex.what());
		} catch (...) {
			LOG("net worker: a result closure threw");
		}
	}
	return batch.size();
}

int Worker::threadMain(void* self) {
	// Below the render thread so a busy network never costs frames.
	Worker* w = static_cast<Worker*>(self);
	if (w->lowPriority_) {
		SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
#ifdef __wii__
		// Nothing more to do on the Wii, and that is verified, not assumed (disassembly of libSDL2.a and libogc.a):
		// SDL_SYS_SetThreadPriority is an empty function there, but SDL threads are pthreads and libogc's
		// __syscall_thread_create starts every pthread at the scheduler's lowest priority (127 in tuxedo terms,
		// which is LWP priority 0). So the worker is already below the render thread without any call. Do NOT
		// "fix" this with LWP_SetThreadPriority(LWP_GetSelf(), n): n = LWP priority, and a high n would put the
		// worker above the main thread. The requested stack size is honoured (pthread_attr_setstacksize; the
		// default would be 64 KB). The marker below shows the real stack address range in the log.
		int marker = 0;
		LOG("net worker: Wii thread started, requested stack %u bytes, a local lives at %p", static_cast<unsigned>(kStackBytes), static_cast<void*>(&marker));
#endif
	}
	w->loop();
	return 0;
}

// Chooses the entry to step next, activating queued entries as the class limits allow. Caller holds the lock.
std::shared_ptr<Worker::Entry> Worker::pickLocked() {
	if (entries_.empty()) return nullptr;
	uint32_t now = SDL_GetTicks();

	// A cancelled entry (or any entry once stopping) must finish through its own step, so it ignores the class limit.
	for (auto& e : entries_) {
		if (!e->active && (stopping_ || e->cancel.load(std::memory_order_relaxed))) {
			e->active = true;
			e->lastRun = now;
		}
	}
	int active[3] = {0, 0, 0};
	for (auto& e : entries_) {
		if (e->active) active[static_cast<int>(e->cls)]++;
	}
	for (int c = 0; c < 3; c++) {
		while (active[c] < kActiveLimit[c]) {
			Entry* best = nullptr;
			for (auto& e : entries_) {
				if (e->active || static_cast<int>(e->cls) != c) continue;
				// Icons: the newest request is the one on screen now. Everything else is first come first served.
				bool better = !best || (c == static_cast<int>(JobClass::Icon) ? e->seq > best->seq : e->seq < best->seq);
				if (better) best = e.get();
			}
			if (!best) break;
			best->active = true;
			best->lastRun = now;
			active[c]++;
		}
	}

	auto oldest = [&](auto&& accept) -> std::shared_ptr<Entry> {
		std::shared_ptr<Entry> best;
		for (auto& e : entries_) {
			if (!e->active || !accept(*e)) continue;
			// Unsigned subtraction so the 32-bit tick counter wrapping is harmless.
			if (!best || static_cast<int32_t>(e->lastRun - best->lastRun) < 0) best = e;
		}
		return best;
	};

	if (auto c = oldest([](const Entry& e) { return e.cancel.load(std::memory_order_relaxed); })) return c;
	int top = 3;
	for (auto& e : entries_) {
		if (e->active) top = std::min(top, static_cast<int>(e->cls));
	}
	if (top != static_cast<int>(JobClass::Background)) {
		auto starved = oldest([&](const Entry& e) {
			return e.cls == JobClass::Background && now - e.lastRun >= static_cast<uint32_t>(kBackgroundSliceMs);
		});
		if (starved) return starved;
	}
	return oldest([&](const Entry& e) { return static_cast<int>(e.cls) == top; });
}

void Worker::loop() {
	int fastStreak = 0;   // consecutive Pending steps that took under 1 ms
	for (;;) {
		std::shared_ptr<Entry> e;
		bool stopping = false;
		{
			Lock l(mutex_);
			for (;;) {
				e = pickLocked();
				if (e) break;
				if (stopping_) return;
				SDL_CondWait(cond_, mutex_);
			}
			stopping = stopping_;
		}

		bool done = false;
		bool fastPending = false;
		// A job that keeps returning Pending after its cancel flag is up must not hold the thread, or it would starve
		// every other job: after kStopSteps such steps it is destroyed as it stands.
		if ((stopping || e->cancel.load(std::memory_order_relaxed)) && ++e->cancelSteps > kStopSteps) {
			LOG("net worker: job %llu (%s) ignored the cancel, dropping it", static_cast<unsigned long long>(e->id), e->tag.c_str());
			done = true;
		} else {
			if (!e->transport) {
				std::unique_ptr<Transport> t = factory_ ? factory_() : nullptr;
				if (!t) t = std::make_unique<NoNetworkTransport>();
				// So a job-level cancel() also ends a backend's long init / DNS wait, not only stop().
				t->setCancel(&e->cancel);
				Lock l(mutex_);
				// stop() may have run between the check above and now, before this transport was visible to it.
				if (stopping_) t->abort();
				e->transport = std::move(t);
			}
			JobContext ctx(*this, e->id, *e->transport, hosts_, &e->cancel, e->progress, StepBudget());
			uint32_t t0 = SDL_GetTicks();
			done = e->job->step(ctx) == JobStep::Done;
			fastPending = !done && SDL_GetTicks() - t0 < 1;
		}

		bool pause = false;
		{
			Lock l(mutex_);
			e->lastRun = SDL_GetTicks();
			if (done) entries_.erase(std::find(entries_.begin(), entries_.end(), e));
			if (fastPending) {
				// Sleep only once every active job has just answered "still pending" instantly, i.e. nothing is
				// waiting for a turn that would do real work.
				int active = 0;
				for (auto& o : entries_) active += o->active;
				if (++fastStreak >= std::max(1, active)) {
					pause = true;
					fastStreak = 0;
				}
			} else {
				fastStreak = 0;
			}
		}
		// A finished job is destroyed here, outside the lock, on the worker thread.
		if (done) e.reset();
		if (pause) SDL_Delay(1);
	}
}

}  // namespace net
