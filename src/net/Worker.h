#pragma once
// The one network worker: a single SDL thread that time-slices resumable jobs cooperatively (the Increment 5a
// decision, rationale in HttpTransfer.h). PC and Wii share this file; only the Transport factory differs.
//
// RULES
//  - ALL HTTP in the app goes through this worker, M3's archive downloads included, as Background jobs. libogc's
//    net_* is not known to be thread-safe, so M3 must not create its own network thread or a netMutex. (M3's install
//    worker, which extracts, hashes and writes the SD card, may exist but never touches a socket.)
//  - The worker thread never calls audio, Text, Ui or Gfx and never creates SDL textures. A job hands results to the
//    main thread with JobContext::post(); App::run() calls drainResults() once per frame.
//  - Jobs and their results never capture `const AppEntry*`, `const AppEntry&` or the Catalog. They copy slug, URLs,
//    sizes and hashes at enqueue time and refer to apps by slug; the main thread re-resolves with catalog.find(slug),
//    where a missing slug is a normal case. That is what lets a Refresh replace the catalog under a running download.
//  - Synchronisation uses SDL_mutex / SDL_cond only (known to work in the Wii SDL2 port), no C++ threading headers.
//
// THREADING MODEL
//  - submit(), cancel(), cancelAll(), dropWhere(), progress(), pending() and drainResults() may be called from any
//    thread (in practice the main thread). Job::step() and everything it owns (sinks, Transport, HostCache, the
//    sink's callbacks, the job's destructor) run only on the worker thread, never concurrently.
//  - Result closures run on the thread that calls drainResults().
//
// SCHEDULING
//  - Classes: Catalog > Icon > Background. Per class only a few jobs are active at once (1 Catalog, 1 Icon, 2
//    Background); the rest wait in a queue (Icon newest first, others first come first served). Each loop iteration
//    runs one step() of one active job: the highest class that has an active job, round-robin inside a class. A
//    Background job that has not run for kBackgroundSliceMs gets a slice anyway, so a download keeps moving while
//    higher-priority work exists. With no jobs the thread sleeps on a condition variable.
//  - Every active job owns one Transport (one connection) created by the factory when it is activated. Jobs chain
//    transfers on it (close + connect again) and share the worker's HostCache, so DNS runs once per host.
//  - Non-sliceable work (DNS, a jansson parse, an SD write) runs to completion inside one step(); a job should log how
//    long it took.
//
// CANCEL AND SHUTDOWN
//  - Each job has a cancel flag (JobContext::cancel). cancel(id), dropWhere() and cancelAll() raise it. A cancelled job
//    is still stepped (even if still queued and over its class limit) so it can finish through its normal path
//    (HttpTransfer reports Cancelled, sinks run onFinish(false), FileSink removes its .tmp); it should return Done
//    promptly. A cancelled job is stepped before any other.
//  - stop() raises every flag, wakes the thread, steps each remaining job to completion (at most kStopSteps steps
//    each, then the job is destroyed as it stands; destroying an HttpTransfer finishes it as Cancelled) and joins.
//    Poll waits are at most ~50 ms and step budgets ~30 ms, so a stop takes well under a second, except while a job is
//    inside blocking DNS (getaddrinfo / net_gethostbyname), which cannot be interrupted.
//  - Undelivered results stay queued; drainResults() still works after stop(), and the destructor discards them.
//  - Once stopped a Worker cannot be restarted and submit() returns 0.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/Http.h"
#include "net/HttpTransfer.h"
#include "net/Transport.h"

struct SDL_mutex;
struct SDL_cond;
struct SDL_Thread;

namespace net {

using JobId = uint64_t;   // 0 = none / rejected

enum class JobClass { Catalog = 0, Icon = 1, Background = 2 };
enum class JobStep { Pending, Done };

// Progress a job publishes for the main thread to read (Worker::progress). 32-bit atomics because the Wii's PPC has
// no 64-bit ones (they would need libatomic). Values saturate at 4 GB - 1, the FAT32 file size limit anyway.
struct JobProgress {
	std::atomic<uint32_t> received{0};
	std::atomic<int32_t> total{-1};   // -1 = unknown
	void set(uint64_t rec, int64_t tot) {
		constexpr uint64_t kMax32 = 0xFFFFFFFFull;
		received.store(static_cast<uint32_t>(rec < kMax32 ? rec : kMax32), std::memory_order_relaxed);
		total.store(tot < 0 ? -1 : static_cast<int32_t>(tot < 0x7FFFFFFF ? tot : 0x7FFFFFFF), std::memory_order_relaxed);
	}
};

class Worker;

// What a job sees while it is stepped. Valid only inside step().
struct JobContext {
	JobContext(Worker& w, JobId i, Transport& t, HostCache& h, CancelFlag c, JobProgress& p, const StepBudget& b)
		: worker(w), id(i), transport(t), hosts(h), cancel(c), progress(p), budget(b) {}
	Worker& worker;
	JobId id;
	Transport& transport;     // this job's connection slot
	HostCache& hosts;
	CancelFlag cancel;
	JobProgress& progress;    // outlives the job; safe to hold in a ProgressFn
	StepBudget budget;

	bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
	void setProgress(uint64_t received, int64_t total) { progress.set(received, total); }
	// Queues `fn` to run on the main thread inside drainResults(). It must not use the job, the sink or the context.
	void post(std::function<void()> fn);
};

class Job {
public:
	virtual ~Job() = default;
	// Advance by at most one ctx.budget (DNS and similar may exceed it). Return Done exactly once the job is over,
	// whatever the outcome, including after ctx.cancelled(). A job holds no AppEntry or Catalog pointers (see above).
	virtual JobStep step(JobContext& ctx) = 0;
};

// A single GET into a sink. `onDone` runs on the worker thread with the sink still alive and the final result, so it
// can commit() a FileSink and then ctx.post() a copy of what the main thread needs.
class HttpGetJob : public Job {
public:
	using DoneFn = std::function<void(HttpSink& sink, const HttpResult& result, JobContext& ctx)>;
	HttpGetJob(HttpRequest req, std::unique_ptr<HttpSink> sink, DoneFn onDone = nullptr);
	~HttpGetJob() override;
	JobStep step(JobContext& ctx) override;

private:
	HttpRequest req_;
	std::unique_ptr<HttpSink> sink_;   // declared before xfer_: the transfer's destructor still calls sink_->onFinish
	DoneFn onDone_;
	std::unique_ptr<HttpTransfer> xfer_;
};

struct JobInfo {
	JobId id = 0;
	JobClass cls = JobClass::Background;
	const std::string& tag;   // the caller's label, e.g. an icon's slug
	bool active = false;
};

class Worker {
public:
	using TransportFactory = std::function<std::unique_ptr<Transport>()>;
	static constexpr int kBackgroundSliceMs = 250;
	static constexpr int kStopSteps = 8;

	// An empty factory (or one returning null) gives jobs a transport that reports NoNetwork, which is what the Wii
	// build does until Increment 7 supplies a libogc one.
	explicit Worker(TransportFactory factory);
	~Worker();
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;

	bool start();
	void stop();
	bool running() const { return running_; }

	// Returns the job's id, or 0 if the worker was stopped (the job is then destroyed on the calling thread).
	JobId submit(JobClass cls, std::string tag, std::unique_ptr<Job> job);
	bool cancel(JobId id);
	void cancelAll();
	// Cancels every queued or active job `pred` accepts and returns how many. `pred` runs under the worker's lock: it
	// must be quick and must not call back into the Worker.
	size_t dropWhere(const std::function<bool(const JobInfo&)>& pred);
	bool progress(JobId id, uint64_t& received, int64_t& total) const;
	size_t pending() const;   // jobs submitted and not yet finished

	// Runs the queued result closures on the calling (main) thread. Returns how many ran.
	size_t drainResults();
	void post(std::function<void()> fn);

private:
	struct Entry;
	static int threadMain(void* self);
	void loop();
	std::shared_ptr<Entry> pickLocked();

	TransportFactory factory_;
	SDL_mutex* mutex_ = nullptr;
	SDL_cond* cond_ = nullptr;
	SDL_Thread* thread_ = nullptr;
	std::vector<std::shared_ptr<Entry>> entries_;
	std::vector<std::function<void()>> results_;
	HostCache hosts_;
	JobId nextId_ = 1;
	uint32_t nextSeq_ = 0;
	bool stopping_ = false;
	std::atomic<bool> running_{false};
};

// The platform's real transport: POSIX sockets on the PC. Empty on the Wii until Increment 7 adds libogc, so every
// job there fails with NoNetwork instead of crashing.
Worker::TransportFactory defaultTransportFactory();

}  // namespace net
