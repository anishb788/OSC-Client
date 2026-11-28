// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// install::Jobs (M3 Increment 7): the job system App owns. It outlives every screen and every Refresh (resetTo and
// refreshCatalog() never touch it), so, like everything that outlives a screen (net/Worker.h), it holds NO AppEntry*, AppEntry&,
// Catalog* or Screen: a JobSpec is a plain-value copy and apps are named by slug; the UI re-resolves them with catalog.find().
//
// THREADING (the Threading rule)
//   main thread   every public member except where noted. Owns the JobQueue (all job state), starts the head of the line when
//                 idle, and in update() (once per frame, right after net::Worker::drainResults()) applies what the worker
//                 reported and builds the snapshot. It never waits for the worker, never does file I/O, never takes a lock for
//                 longer than a vector swap: update() is O(jobs).
//   install worker  (InstallWorker.h) runs one job at a time, DISK work only, in the task update() handed it. It talks back
//                 through (1) the job's ProgressCell, 32-bit atomics: stage, bytes, and the cancel / in-commit phase; and (2) ONE
//                 mutex-protected vector of finished results, which update() swaps out. The mutex also guards the set of staging
//                 dirs claimed by live jobs (what Cleanup must not touch). No other shared state.
//   net worker    Increment 8 submits the download there; this file opens no sockets.
// SNAPSHOT: update() rebuilds `snapshot()`, a plain value (strings, ints), from the queue and the progress cells. The UI reads only
// that, so no screen touches a cell, an entry or a lock. Valid until the next update().
//
// RUNNERS: what a job kind DOES lives in a Runner: a function run on the install worker. Increment 7 provides Cleanup and the dev
// Sleep; Increments 8 and 8a install the Install/Update runner (download hand-off, verify, extract, commit), 12 Delete, 11 Scan.
// A kind without a runner fails at start with "not available yet". Before an Install, Update or Delete runner runs, the wrapper
// checks that no journal is present on the job's root (an interrupted job must be recovered first; writeStartJournal would
// overwrite it) and fails the job with the reason otherwise.
//
// RUNNER CONTRACTS (what Increments 8, 8a and 12 must keep)
//   * Commit. A runner that changes ANYTHING outside its staging dir (the first rename into apps/, a manifest, the journal's commit
//     phase, a delete) must have called ctx.cell.enterCommit() first, and must give up when that returns false (the job was
//     cancelled before). Once the runner has returned, the task wrapper marks the cell finished (ProgressCell::markFinished): from
//     then on cancel() is refused and the job ends with the runner's own result. A runner that returns Done without having called
//     enterCommit() must not have changed anything outside staging (a cancel that arrived before the return turns the job into
//     Cancelled whatever the runner says, so the books would say "cancelled" about a job that did change the card).
//   * Staging claim. A job that creates <root>/apps/OpenShop/staging/<id> claims it with ctx.claimStaging(root, id) BEFORE the dir
//     exists (and before the start journal is written); the claim ends when the runner returns. The Cleanup job (cleanStaging with
//     the claims of that root as CleanGuard::activeJobIds) never touches a claimed dir. The Install job (Increments 8/8a) is the
//     RUNNING HEAD of the FIFO for its whole life, including the net-worker download phase: it claims its dir and keeps the
//     journal check for as long as it runs, so a Cleanup job can never run concurrently on the install worker. The claim is the
//     second line of defence (and what the probe `claimprobe=1` exercises on the device): ctx.cleanup() runs the Cleanup routine
//     inside a runner, while that runner holds a claim.
//   * add() refuses (returns 0) once the job system is not running (never started, start() failed, or shutting down). Callers
//     (Increments 8, 9, 12) answer with "not available" and queue nothing.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/FileUtil.h"
#include "install/InstallWorker.h"
#include "install/JobQueue.h"

namespace install {

// What a runner reports. Done, Failed (with a UI-ready text) or Cancelled.
struct TaskOutcome {
	JobState state = JobState::Done;
	std::string error;
};

// What a runner may use. Worker thread only.
struct RunCtx {
	ProgressCell& cell;
	JobId id = 0;
	// Marks <root>/apps/OpenShop/staging/<jobDirId> as the dir of a live job, so the Cleanup job leaves it alone (released when the
	// runner returns). Claim BEFORE creating the dir (and before the start journal is written).
	std::function<void(const std::string& root, int64_t jobDirId)> claimStaging;
	// Runs the Cleanup routine (the one the Cleanup job runs: cleanStaging with the claims of `root` as active ids, polling this job's
	// cancel flag) right now, inside this runner. Dev probe and tests; a normal runner never needs it.
	std::function<TaskOutcome(const std::string& root, const std::string& device)> cleanup;
};
using Runner = std::function<TaskOutcome(const JobSpec&, RunCtx&)>;

struct JobView {
	JobId id = 0;
	JobKind kind = JobKind::Sleep;
	JobState state = JobState::Queued;
	std::string slug, name, version;
	const char* stage = ""; // UI-ready label of what the worker reports ("" while Queued or finished)
	std::string error;      // Failed
	uint64_t done = 0, total = 0;
	bool inCommit = false;  // cannot be cancelled now
	int position = -1;      // USER jobs only: 0 = the first user job (running or next, whatever Cleanup does ahead of it), -1 for
	                        // internal jobs and in the history
	bool internal = false;  // Cleanup / Scan: the UI does not list these
	bool cancelling = false; // cancelled, the worker has not stopped yet
};
struct JobSnapshot {
	std::vector<JobView> line;    // running first, then queued
	std::vector<JobView> history; // newest first, at most kHistoryMax
	size_t active = 0;            // running + queued, user jobs only (the top-bar badge)
	JobId next = 0;               // the first queued USER job
	bool busy = false;
	uint32_t seq = 0;             // bumped on every rebuild
};

// "" when nothing blocks a job from starting on `root`: the journal is cleanly missing (lstat ENOENT on ".journal" and its
// backup). Otherwise the reason, UI-ready: the restart text for a readable journal that recovery has not dealt with yet, a
// different text (what to remove, or to use the other device) for one that is corrupt, foreign, unreadable or only a backup.
// Does disk I/O: worker thread (or a test) only.
std::string journalBlocker(const std::string& root, const std::string& device, const FileOps& ops = {});

class Jobs {
public:
	struct Config {
		std::vector<std::string> removeRoots; // FileUtil::removeTree's roots (platform::storageRoots() in the app)
		size_t stackBytes = 64 * 1024;
		bool measureStack = false;            // perf=1: log the worker's stack high-water at exit
		bool logProgress = false;             // log progress lines of running jobs (from the snapshot, ~1/s)
		FileOps fileOps;                      // tests
		std::function<uint32_t()> nowMs;      // default SDL_GetTicks
		uint32_t commitWaitMs = 30000;        // shutdown waits this long for a job that is committing
		uint32_t exitWaitMs = 1500;           // ... and this long for one that is not (it stops within a chunk)
	};

	Jobs();
	~Jobs(); // shutdown() if it was not called

	bool start(const Config& cfg);
	bool running() const { return started_; }
	// Phase 1 of shutdown, does not wait: refuses new jobs, drops queued ones, requests cancel of the running one (unless it is
	// committing) and asks the worker to stop. App::shutdown calls it, then asks the net worker to cancel, then shutdown(), so the
	// waits overlap. Idempotent.
	void beginShutdown();
	// beginShutdown(), then waits (bounded: commitWaitMs while committing, exitWaitMs otherwise) for the worker and joins it. A
	// worker that does not stop in time is detached and logged; the journal makes a cut commit recoverable. Main thread.
	// (Increment 16 moves this wait out of App::shutdown into the pending-quit state.)
	void shutdown();

	// ---- the pipeline driver ----
	void update(); // once per frame

	// ---- queue (main thread) ----
	// 0 = refused: the job system is not running (never started, start() failed, or shutting down); nothing was queued, so busy()
	// cannot get stuck. Callers show "not available". Adding a USER job while a Cleanup job is running requests that Cleanup's
	// cancel (it is retried at the next start), so a user's job never waits behind housekeeping.
	JobId add(JobKind kind, JobSpec spec);
	// Dev: like add(), but `r` runs this one job instead of the kind's registered runner (the claim probe; tests).
	JobId addWithRunner(JobKind kind, JobSpec spec, Runner r);
	// Internal housekeeping: removes abandoned staging dirs under <root>/apps/OpenShop/staging (cleanStaging with its guard).
	// Queued per mounted root at start-up after recovery, never for a root whose recovery was LeftAlone or Failed.
	JobId queueCleanup(const std::string& root, const std::string& device);
	bool cancel(JobId id) { return q_.cancel(id); }
	bool remove(JobId id) { return q_.remove(id); }
	JobId retry(JobId id, JobSpec spec); // 0 when `id` is not a Failed history entry or the job system is not running
	void clearFinished() { q_.clearFinished(); }
	// The sum of: any queued or running job but Cleanup. Backs AppsDeviceSetting::setBusy (user decision 2026-11-25).
	// False when the system is not running (see add()).
	bool busy() const { return started_ && !stopping_ && q_.busy(); }
	const JobQueue& queue() const { return q_; }
	const JobSnapshot& snapshot() const { return snap_; }
	void setRunner(JobKind kind, Runner r);

	// Dev: queues `n` Sleep jobs; the last fails when `failLast`. `commit` makes the second half of each count as the commit phase.
	void queueDevSleeps(int n, uint32_t ms, bool failLast, bool commit, const std::string& root);

	size_t stackHighWater() const { return worker_.stackHighWater(); } // valid after shutdown() with measureStack

private:
	struct Shared;
	void buildSnapshot();
	void startHead();
	void applyResults();
	void logProgress();
	JobView view(const JobEntry& e) const;

	JobQueue q_;
	JobSnapshot snap_;
	Config cfg_;
	InstallWorker worker_;
	std::shared_ptr<Shared> shared_;
	std::map<JobKind, Runner> runners_;
	std::function<TaskOutcome(const JobSpec&, ProgressCell&)> cleanFn_; // the Cleanup routine (owns refs only; safe for a detached worker)
	std::map<JobId, Runner> jobRunners_; // addWithRunner overrides, consumed when the job starts
	bool started_ = false, stopping_ = false;
	uint32_t shutdownT0_ = 0;
	bool stuckAtShutdown_ = false;
	uint32_t lastLogMs_ = 0;
	std::map<JobId, JobState> logged_; // last state logged per job (transitions only)
};

} // namespace install
