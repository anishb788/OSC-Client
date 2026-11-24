#pragma once
// A resumable HTTP GET: a state machine over a non-blocking Transport, advanced in bounded slices by step().
//
// DECISION (Increment 5a): one network worker thread time-slices these transfers cooperatively. Between slices the
// worker picks the highest-priority runnable job, so a catalog fetch or icon request is serviced while an M3 archive
// download stays open. A paused download keeps its socket open and TCP flow control throttles the server for the few
// hundred ms the higher-priority work takes, so no HTTP Range support is needed. M3's downloads run on this worker as
// Background jobs; M3 adds no network thread of its own and no netMutex.
//
// Why not the alternatives:
//  - A second download worker behind a netMutex: it depends on libogc net_* thread safety, which is undocumented (the
//    reason every net call is kept on one thread), adds a second LWP stack and invites priority inversion between LWP
//    priorities.
//  - Pause/resume through HTTP Range: needs Range support on the server (unverified on hbb1), a reconnect and
//    partial-file bookkeeping per pause, and still blocks whenever a transfer isn't at a resumable point. Range may
//    still be added by M3 later for a different purpose (resuming after an app restart).
//
// Timing rules:
//  - step() is bounded by StepBudget (body bytes and milliseconds, socket waits included), with one exception: DNS
//    (Transport::resolve) is blocking and cannot be sliced. A per-worker HostCache makes it run once per host, and its
//    duration is logged.
//  - The connect and idle timers are charged only for time spent inside step(), so a transfer the scheduler leaves
//    alone never times out because of the pause. totalTimeoutMs is wall-clock from the first step (0 = none).
//  - Sink callbacks, the progress callback and the Transport all run on the thread that calls step().
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/Http.h"
#include "net/HttpParse.h"
#include "net/Transport.h"

struct StepBudget {
	size_t maxBytes = 64 * 1024;   // at most this many bytes read from the socket per step
	int maxMs = 30;                // soft wall-clock cap per step (a single sink write or DNS lookup may exceed it)
};

// Resolved IPv4 addresses by host name, so DNS runs once per host. Small and bounded; owned by the worker.
class HostCache {
public:
	bool find(const std::string& host, uint32_t& ipv4) const;
	void put(const std::string& host, uint32_t ipv4);
	void erase(const std::string& host);   // after a connect failure, so a stale address isn't reused all session
private:
	struct Entry { std::string host; uint32_t ipv4 = 0; };
	static constexpr size_t kMax = 8;
	std::vector<Entry> entries_;
	size_t next_ = 0;
};

class HttpTransfer {
public:
	enum class StepResult { Pending, Done };

	// `sink`, `transport`, `cancel` and `cache` must outlive the transfer, and so must the sink because the destructor
	// of an unfinished transfer finishes it as Cancelled ("abandoned") and calls sink.onFinish(false). `cache` may be
	// null (resolve every time).
	HttpTransfer(const HttpRequest& req, HttpSink& sink, ProgressFn progress, CancelFlag cancel, Transport& transport,
	             HostCache* cache = nullptr);
	~HttpTransfer();
	HttpTransfer(const HttpTransfer&) = delete;
	HttpTransfer& operator=(const HttpTransfer&) = delete;

	// Advances by at most one budget. Calling it after Done just returns Done. The sink's onFinish() has run exactly
	// once by the time Done is returned, whatever the outcome.
	StepResult step(const StepBudget& budget = StepBudget());
	bool done() const { return phase_ == Phase::Done; }
	// Valid once done().
	const HttpResult& result() const { return result_; }

private:
	enum class Phase { Start, Resolve, Connect, Send, Receive, Done };

	StepResult finish(HttpError err, const std::string& detail);
	void charge();                         // adds time since the last mark to the connect or idle timer
	bool timedOut(HttpError& err, std::string& detail);
	void waitFor(bool forWrite);
	void reportProgress(bool force);
	void dropForeignHeaders();
	bool onResponseDone();                 // handles a finished parse; returns true if the transfer is over

	HttpRequest req_;
	HttpSink& sink_;
	ProgressFn progress_;
	CancelFlag cancel_;
	Transport& tr_;
	HostCache* cache_;

	Phase phase_ = Phase::Start;
	httpparse::Url url_;
	std::unique_ptr<httpparse::ResponseParser> parser_;
	std::string wire_;                     // the request being sent
	size_t sent_ = 0;
	int redirects_ = 0;
	uint32_t addr_ = 0;

	uint64_t startMs_ = 0;                 // wall-clock start (first step), for totalTimeoutMs
	uint64_t mark_ = 0;                    // last time the timers were charged
	uint64_t deadline_ = 0;                // end of the current step's time budget
	uint64_t connectMs_ = 0;               // time stepped while connecting
	uint64_t idleMs_ = 0;                  // time stepped since the last byte moved
	uint64_t lastProgressMs_ = 0;
	uint64_t lastProgressBytes_ = 0;
	bool progressSent_ = false;

	HttpResult result_;
};

// Blocking convenience: steps one transfer to completion. For tools, tests and simple callers; the app's worker
// steps transfers itself.
HttpResult httpGet(const HttpRequest& req, HttpSink& sink, ProgressFn progress, CancelFlag cancel, Transport& transport,
                   HostCache* cache = nullptr);
