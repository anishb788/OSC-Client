#pragma once
// The one network worker: a single SDL thread that time-slices resumable jobs cooperatively (the Increment 5a
// decision, rationale in HttpTransfer.h). PC and Wii share this file; only the Transport factory differs.
//
// RULES
//  - ALL HTTP in the app goes through this worker, M3's archive downloads included, as Background jobs. libogc's
//    net_* is not known to be thread-safe, so M3 must not create its own network thread or a netMutex. (M3's install
//    worker, which extracts, hashes and writes the SD card, may exist but never touches a socket.)
//    The one deliberate exception is the Wii's net_gethostbyname, which runs on a detached DNS thread (HttpWii.cpp) so
//    the worker can abandon a lookup it cannot interrupt. That is safe enough because each net_* call is a separate IOS
//    ioctl with its own buffers, and HttpWii allows only one live lookup at a time, which protects the static buffer
//    net_gethostbyname returns. Everything else stays on the worker thread.
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
//  - Result closures run on the thread that calls drainResults(). If exceptions are enabled in the build, one that
//    escapes a closure is logged and the rest of the batch still runs.
//  - start() and stop() (and so the destructor) are for the main thread only; they are not safe to race with each other.
//  - Anything a job's callbacks (HttpGetJob's onDone included) or a posted closure captures by reference must outlive
//    stop(): onDone also runs for Cancelled and stopped transfers, and stop() still steps every job. Declare that state
//    before the Worker, so it is destroyed after it.
//
// SCHEDULING
//  - Classes: Catalog > Icon > Background. Per class only a few jobs are active at once (1 Catalog, 1 Icon, 2
//    Background); the rest wait in a queue (Icon newest first, others first come first served). Each loop iteration
//    runs one step() of one active job: the highest class that has an active job, round-robin inside a class. A
//    Background job that has not run for kBackgroundSliceMs gets a slice anyway, so a download keeps moving while
//    higher-priority work exists. With no jobs the thread sleeps on a condition variable.
//  - A step that returns Pending in under 1 ms, with no other active job to step, is followed by a 1 ms sleep, so a job
//    that polls without waiting cannot spin the CPU. A job should still wait inside its Transport.
//  - Two active Background jobs block-wait in turn (each step's Transport::wait can use up to its slice), so one
//    stalled download delays the other by that wait; a multi-descriptor wait is a future improvement.
//  - Every active job owns one Transport (one connection) created by the factory when it is activated. Jobs chain
//    transfers on it (close + connect again) and share the worker's HostCache, so DNS runs once per host.
//  - Non-sliceable work (DNS, a jansson parse, an SD write) runs to completion inside one step(); a job should log how
//    long it took.
//
// CANCEL AND SHUTDOWN
//  - Each job has a cancel flag (JobContext::cancel). cancel(id), dropWhere() and cancelAll() raise it. A cancelled job
//    is still stepped (even if still queued and over its class limit) so it can finish through its normal path
//    (HttpTransfer reports Cancelled, sinks run onFinish(false), FileSink removes its .tmp); it should return Done
//    promptly. A cancelled job is stepped before any other. The job's transport also gets the flag
//    (Transport::setCancel), so a backend's long init or DNS wait ends at once instead of stalling the worker.
//  - stop() raises every flag, wakes the thread, steps each remaining job to completion (at most kStopSteps steps
//    each, then the job is destroyed as it stands; destroying an HttpTransfer finishes it as Cancelled) and joins. A
//    job that ignores a plain cancel() gets the same kStopSteps steps after the flag is raised, then is destroyed too
//    (its onDone does not run then), so it cannot starve the others.
//    Poll waits are at most ~50 ms and step budgets ~30 ms, so a stop takes well under a second, except while a job is
//    inside blocking DNS (getaddrinfo on the PC, where stop() waits it out). On the Wii the lookup runs on a throwaway
//    thread and Transport::abort(), which stop() calls before joining, makes the worker walk away from it.
//  - Undelivered results stay queued; drainResults() still works after stop(), and the destructor discards them.
//  - Once stopped a Worker cannot be restarted and submit() returns 0. Neither does it accept jobs before a successful
//    start(): callers must treat a 0 from submit() as "this job will never run" and show an error, not wait.
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

// IconLocal is for an icon's SD card read, which finishes in one short step: its own lane, so a cached icon never waits
// behind a network download that is stalled in the single Icon slot.
enum class JobClass { Catalog = 0, IconLocal = 1, Icon = 2, Background = 3 };
enum class JobStep { Pending, Done };

// Progress a job publishes for the main thread to read (Worker::progress). 32-bit atomics because the Wii's PPC has
// no 64-bit ones (they would need libatomic). Both values saturate at 4 GB, the FAT32 file size limit anyway: received
// at 0xFFFFFFFF, total at 0xFFFFFFFE because 0xFFFFFFFF in `total` means unknown.
struct JobProgress {
	static constexpr uint32_t kUnknown = 0xFFFFFFFFu;
	std::atomic<uint32_t> received{0};
	std::atomic<uint32_t> total{kUnknown};
	void set(uint64_t rec, int64_t tot) {
		received.store(static_cast<uint32_t>(rec < 0xFFFFFFFFull ? rec : 0xFFFFFFFFull), std::memory_order_relaxed);
		total.store(tot < 0 ? kUnknown : static_cast<uint32_t>(tot < 0xFFFFFFFELL ? tot : 0xFFFFFFFELL), std::memory_order_relaxed);
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

	// An empty factory (or one returning null) gives jobs a transport that reports NoNetwork.
	// `lowPriority` runs the thread at SDL_THREAD_PRIORITY_LOW, below the render thread; tests turn it off because a
	// nice-19 thread starves under CPU load and makes timing checks flaky.
	explicit Worker(TransportFactory factory, bool lowPriority = true);
	~Worker();
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;

	// Main thread only. start() fails (false) if the thread cannot be created, if already started, or after stop().
	bool start();
	void stop();
	bool running() const { return running_; }

	// Returns the job's id, or 0 unless the worker thread is running (never started, start() failed, or stopped); the
	// job is then destroyed on the calling thread and will never run. Callers must treat 0 as failure.
	JobId submit(JobClass cls, std::string tag, std::unique_ptr<Job> job);
	// True if the job is still queued or running, including one that was already cancelled (cancelling twice is fine).
	bool cancel(JobId id);
	void cancelAll();
	// Cancels every queued or active job `pred` accepts and returns how many. Jobs that were already cancelled are not
	// offered to `pred` and not counted. `pred` runs under the worker's lock: it must be quick and must not call back
	// into the Worker.
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
	bool lowPriority_ = true;
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

// The platform's real transport: POSIX sockets on the PC, libogc net_* on the Wii.
Worker::TransportFactory defaultTransportFactory();

}  // namespace net
