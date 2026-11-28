// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The pure job queue model (M3 Increment 7). No SDL, no disk, no threads: host-tested in install_test. install::Jobs (Jobs.h)
// owns one of these, drives it from the main thread and feeds it the results of the install worker.
//
// RULES
//   * MAIN THREAD ONLY. Nothing here is locked. The only state shared with the worker is a ProgressCell (atomics, reference
//     counted), so a worker task can outlive a removed entry.
//   * No pointers into the catalog (M2 decision, Worker.h): a JobSpec is a plain-value copy and apps are named by slug.
//   * FIFO. Delete shares the queue (no jump-ahead). One job runs at a time.
//   * History: the 20 newest Done / Failed USER jobs (never Cleanup or Scan), newest first, not persisted. A cancelled or removed
//     job leaves no entry.
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace install {

using JobId = uint32_t; // 0 = none

enum class JobKind {
	Install,
	Update,
	Delete,  // the code may say Uninstall; the UI never does
	Cleanup, // internal: removes abandoned staging (Increment 7)
	Scan,    // internal: the installed-apps scan (Increment 11)
	Sleep,   // dev only (devjobs=<n>)
};
const char* kindName(JobKind k);
// Cleanup and Scan are the app's own housekeeping: never in the history, never in the user-facing counts.
inline bool isInternal(JobKind k) { return k == JobKind::Cleanup || k == JobKind::Scan; }

enum class JobState {
	Queued,
	Downloading,
	Verifying,
	Installing,
	AwaitingConfirmation, // wired in Increment 10
	Finalizing,
	Done,
	Failed,
	Cancelled,
};
const char* stateName(JobState s);
inline bool isFinished(JobState s) { return s == JobState::Done || s == JobState::Failed || s == JobState::Cancelled; }

// What the worker reports about itself (an atomic in the ProgressCell). The state the UI shows for a running job follows from it.
enum class Stage : uint8_t {
	Waiting,    // handed to the worker, nothing reported yet
	Checking,   // the journal check before the job writes anything
	Downloading,
	Verifying,
	Extracting,
	Committing,
	Finalizing,
	Cleaning,   // Cleanup job
	Sleeping,   // dev Sleep job
};
const char* stageLabel(Stage s); // UI-ready ("Downloading", "Installing", ...)
JobState stateForStage(Stage s);

enum class DeleteScope : uint8_t { OnlyInstalled, Everything };

// Everything a job needs, copied at queue time. Later increments fill the install fields; Cleanup and Sleep use only what is noted.
struct JobSpec {
	std::string slug, name, version;
	std::string url, md5;           // the rebuilt, allowlisted archive URL and the catalog's MD5 (Increment 8)
	uint64_t archiveSize = 0, uncompressedSize = 0;
	uint8_t packageType = 0;        // Catalog's PackageType value
	std::vector<std::string> dirs;  // the catalog's subdirectories hint (never a delete list)
	std::string targetRoot;         // the device root the job acts on ("sd:", "./sdcard", ...), copied at queue time
	std::string device;             // "sd" | "usb": which device targetRoot is (the journal is bound to it)
	uint32_t confirmedFlags = 0;    // the warnings the user confirmed (Increment 10)
	DeleteScope deleteScope = DeleteScope::OnlyInstalled;
	// Cleanup: targetRoot and device only. Sleep (dev): how long, whether the last stretch counts as a commit, whether it fails.
	uint32_t sleepMs = 3000;
	bool sleepCommit = false;
	bool sleepFail = false;
};

// Shared between the main thread and the one worker task that runs the job. 32-bit atomics only (the Wii is 32-bit PowerPC).
// Cancel and "in commit" are ONE atomic so that they exclude each other: a job that has begun to commit can no longer be
// cancelled, and a cancelled job can no longer begin to commit.
class ProgressCell {
public:
	// ---- worker side ----
	void setStage(Stage s) { stage_.store(static_cast<uint8_t>(s), std::memory_order_relaxed); }
	void setProgress(uint64_t done, uint64_t total) {
		done_.store(sat(done), std::memory_order_relaxed);
		total_.store(sat(total), std::memory_order_relaxed);
	}
	bool cancelled() const { return phase_.load(std::memory_order_acquire) == kCancelled; }
	// Call just before the first change outside staging. False: the job was cancelled first, so do not commit.
	bool enterCommit() {
		int expect = kRunning;
		return phase_.compare_exchange_strong(expect, kCommit, std::memory_order_acq_rel) || expect == kCommit;
	}
	// The task wrapper calls this when the runner has RETURNED (whatever it returned). From then on requestCancel() fails: a result
	// that is already decided cannot be cancelled afterwards. A cancelled or committing cell keeps its phase.
	void markFinished() {
		int expect = kRunning;
		phase_.compare_exchange_strong(expect, kFinished, std::memory_order_acq_rel);
	}
	// ---- main thread side ----
	Stage stage() const { return static_cast<Stage>(stage_.load(std::memory_order_relaxed)); }
	uint64_t done() const { return done_.load(std::memory_order_relaxed); }
	uint64_t total() const { return total_.load(std::memory_order_relaxed); }
	bool inCommit() const { return phase_.load(std::memory_order_acquire) == kCommit; }
	bool finished() const { return phase_.load(std::memory_order_acquire) == kFinished; }
	// True when the flag is set now or was set before. False when the job is committing (enterCommit won) or its runner has already
	// returned (markFinished): in both cases the job will end with the result it has.
	bool requestCancel() {
		int expect = kRunning;
		return phase_.compare_exchange_strong(expect, kCancelled, std::memory_order_acq_rel) || expect == kCancelled;
	}

private:
	static uint32_t sat(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }
	enum : int { kRunning = 0, kCancelled = 1, kCommit = 2, kFinished = 3 };
	std::atomic<uint8_t> stage_{static_cast<uint8_t>(Stage::Waiting)};
	std::atomic<uint32_t> done_{0}, total_{0};
	std::atomic<int> phase_{kRunning};
};

struct JobEntry {
	JobId id = 0;
	JobKind kind = JobKind::Sleep;
	JobSpec spec;
	JobState state = JobState::Queued;
	std::string error; // Failed: UI-ready text
	std::shared_ptr<ProgressCell> cell;
	bool running = false; // handed to the worker and not finished yet (a cancelled running job stays here until the worker is done)
};

constexpr size_t kHistoryMax = 20;

class JobQueue {
public:
	// Adds a Queued job at the tail. Never returns 0.
	JobId add(JobKind kind, JobSpec spec);

	// The head of the line, when nothing is running and it is Queued; null otherwise. Does not change anything.
	const JobEntry* nextToStart() const;
	// Marks `id` (the one nextToStart returned) as running. Its state follows the stage it reports (update). False for any other id.
	bool markStarted(JobId id);
	// Copies the stage the running job reports into its state (unless it was cancelled).
	void syncRunning();
	// The worker finished `id` with `outcome` (Done, Failed or Cancelled). A job that was cancelled meanwhile stays Cancelled whatever
	// the worker says. User jobs that end Done or Failed go to the history; every finished job leaves the line. False for an unknown id.
	bool finish(JobId id, JobState outcome, const std::string& error);

	// Queued: removed, no history entry, true. Running and not committing: flagged (the worker notices within a chunk), state
	// Cancelled now, the entry stays until finish() (so the next job waits for it), true. Running in commit, already cancelled,
	// finished or unknown: false.
	bool cancel(JobId id);
	// Cancel every queued job and request cancel of the running one; returns true if a running job could not be cancelled
	// (it is committing). Used at shutdown.
	bool cancelAll();
	// A Queued job or a history entry. A running job is cancelled instead (false here).
	bool remove(JobId id);
	// A Failed history entry: queues a new job of the same kind with `spec` (the caller re-resolved it) and drops the old entry.
	// 0 if `id` is not a Failed history entry.
	JobId retry(JobId id, JobSpec spec);
	void clearFinished();

	// ---- queries ----
	const JobEntry* find(JobId id) const;           // line or history
	const std::deque<JobEntry>& line() const { return line_; }       // running first, then Queued, in order
	const std::deque<JobEntry>& history() const { return history_; } // newest first
	const JobEntry* running() const;                 // the job handed to the worker (even if cancelled), or null
	// running + queued, all kinds / user kinds only (what the top-bar badge counts).
	size_t activeCount() const { return line_.size(); }
	size_t userActiveCount() const;
	// USER jobs only (user decision 2026-11-27, review M1): 0 = the first user job (running or next), 1 = one behind it, ...; an
	// internal job (Cleanup, Scan) ahead of it does not count, so a user job queued behind Cleanup is position 0 / "next". -1 for an
	// internal job and when `id` is not in the line.
	int position(JobId id) const;
	// The first Queued USER job (not the running one, never an internal one), or 0.
	JobId nextQueued() const;
	// True while anything but a Cleanup job is queued or running. Backs AppsDeviceSetting's busy guard (user decision 2026-11-25):
	// the apps device cannot be switched under a job. Cleanup works on every mounted root regardless of the setting, so it does not
	// count.
	// TODO(Increment 11): decide whether Scan counts. Today it does (anything but Cleanup). The reviewer's view: leave Scan out like
	// Cleanup and drop stale results through AppsDeviceSetting::epoch(), because the refusal text "Finish or cancel downloads first"
	// would show with nothing to cancel (user decision 2026-11-27: decide in Increment 11).
	bool busy() const;

private:
	JobId nextId_ = 1;
	std::deque<JobEntry> line_, history_;
	void pushHistory(JobEntry e);
};

} // namespace install
