// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Jobs.h"

#include <algorithm>
#include <exception>

#include <SDL_mutex.h>
#include <SDL_timer.h>

#include "core/Log.h"
#include "install/Journal.h"
#include "install/Recovery.h"

namespace install {

namespace {
struct Lock {
	explicit Lock(SDL_mutex* m) : m_(m) { SDL_LockMutex(m_); }
	~Lock() { SDL_UnlockMutex(m_); }
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
	SDL_mutex* m_;
};

constexpr uint32_t kSleepChunkMs = 50; // a Sleep job looks at its cancel flag this often ("within one chunk interval")

bool writesJournal(JobKind k) { return k == JobKind::Install || k == JobKind::Update || k == JobKind::Delete; }
} // namespace

std::string journalBlocker(const std::string& root, const std::string& device, const FileOps& ops) {
	// A readable journal that this start's recovery has not dealt with: restarting helps.
	static const char* const kRestart =
	    "An earlier install was interrupted and has not been finished yet. Restart OpenShop to recover it.";
	// Restarting does not help here: recovery leaves a journal it cannot read (or that belongs to the other device) alone.
	static const char* const kUnreadable =
	    "The apps folder on this device has an unfinished install that OpenShop can't read. "
	    "Remove apps/OpenShop/installed/.journal, or use the other device.";
	const JournalLoad jl = loadJournal(root, device, ops);
	if (jl.status == LoadStatus::Corrupt) return kUnreadable; // corrupt, unreadable or for another device: never overwritten
	if (jl.status != LoadStatus::Missing) return kRestart;    // present and readable
	const std::string jp = journalPath(root);
	// "Missing" must be a clean ENOENT on both files; a stat error, or a backup with no journal, cannot be told apart from damage.
	if (presenceOf(jp, ops) != Presence::Absent || presenceOf(jp + ".bak", ops) != Presence::Absent) return kUnreadable;
	return {};
}

struct Jobs::Shared {
	struct Result {
		JobId id = 0;
		JobState state = JobState::Done;
		std::string error;
	};
	struct Claim {
		JobId owner = 0;
		std::string root;
		int64_t dirId = 0;
	};
	SDL_mutex* mutex = SDL_CreateMutex();
	std::vector<Result> results; // worker -> main
	std::vector<Claim> claims;   // staging dirs of live jobs
	~Shared() {
		if (mutex) SDL_DestroyMutex(mutex);
	}
};

Jobs::Jobs() = default;
Jobs::~Jobs() { shutdown(); }

bool Jobs::start(const Config& cfg) {
	if (started_) return false;
	stopping_ = false;
	cfg_ = cfg;
	if (!cfg_.nowMs) cfg_.nowMs = [] { return (uint32_t)SDL_GetTicks(); };
	shared_ = std::make_shared<Shared>();
	if (!shared_->mutex) return false;

	// Cleanup: the first real job.
	std::shared_ptr<Shared> sh = shared_;
	const std::vector<std::string> removeRoots = cfg_.removeRoots;
	const FileOps ops = cfg_.fileOps;
	// The Cleanup routine: ONE function, used by the Cleanup job and by RunCtx::cleanup (probe and tests). The guard's active ids are
	// the claims of the job's root at this moment (read under the mutex), and its stop poll is the calling job's cancel flag.
	cleanFn_ = [sh, removeRoots, ops](const JobSpec& s, ProgressCell& cellRef) {
		CleanGuard g;
		g.device = s.device;
		{
			Lock l(sh->mutex);
			for (const Shared::Claim& c : sh->claims)
				if (c.root == s.targetRoot) g.activeJobIds.push_back(c.dirId);
		}
		ProgressCell* cell = &cellRef;
		g.stop = [cell] { return cell->cancelled(); };
		CleanStats st;
		const bool ok = cleanStaging(s.targetRoot, removeRoots, g, ops, &st);
		if (st.stopped) return TaskOutcome{JobState::Cancelled, {}};
		if (st.removed || st.skippedJournal || st.skippedActive || st.failed)
			LOG("cleanup[%s]: removed %d, kept %d (journal %d, active %d), failed %d", s.device.c_str(), st.removed,
			    st.skippedJournal + st.skippedActive, st.skippedJournal, st.skippedActive, st.failed);
		if (!ok) return TaskOutcome{JobState::Failed, "Staging cleanup is incomplete; it is retried at the next start."};
		return TaskOutcome{};
	};
	const auto cleanFn = cleanFn_;
	runners_[JobKind::Cleanup] = [cleanFn](const JobSpec& s, RunCtx& ctx) {
		ctx.cell.setStage(Stage::Cleaning);
		return cleanFn(s, ctx.cell);
	};
	// The dev Sleep job: chunked, cancellable, optionally "in commit" for its second half, optionally failing.
	runners_[JobKind::Sleep] = [](const JobSpec& s, RunCtx& ctx) {
		ctx.cell.setStage(Stage::Sleeping);
		const uint32_t total = s.sleepMs ? s.sleepMs : 1;
		bool committing = false;
		for (uint32_t t = 0; t < total;) {
			if (ctx.cell.cancelled()) return TaskOutcome{JobState::Cancelled, {}};
			if (s.sleepCommit && !committing && t >= total / 2) {
				if (!ctx.cell.enterCommit()) return TaskOutcome{JobState::Cancelled, {}};
				committing = true;
				ctx.cell.setStage(Stage::Committing);
			}
			const uint32_t step = std::min(kSleepChunkMs, total - t);
			SDL_Delay(step); // yields the CPU between chunks
			t += step;
			ctx.cell.setProgress(t, total);
		}
		if (s.sleepFail) return TaskOutcome{JobState::Failed, "Dev job failed on purpose."};
		return TaskOutcome{};
	};

	InstallWorker::Options wo;
	wo.stackBytes = cfg_.stackBytes;
	wo.measureStack = cfg_.measureStack;
	if (!worker_.start(wo)) return false;
	started_ = true;
	return true;
}

void Jobs::setRunner(JobKind kind, Runner r) { runners_[kind] = std::move(r); }

JobId Jobs::add(JobKind kind, JobSpec spec) {
	if (!started_ || stopping_) return 0;
	// A user's job never waits behind housekeeping: the running Cleanup is asked to stop (it ends Cancelled, leaves any dir it was in
	// partly deleted, and runs again at the next start).
	if (!isInternal(kind))
		if (const JobEntry* r = q_.running(); r && r->kind == JobKind::Cleanup && r->state != JobState::Cancelled) {
			LOG("Cleanup: job %u asked to stop, a user job was queued", r->id);
			q_.cancel(r->id);
		}
	return q_.add(kind, std::move(spec));
}

JobId Jobs::addWithRunner(JobKind kind, JobSpec spec, Runner r) {
	const JobId id = add(kind, std::move(spec));
	if (id && r) jobRunners_[id] = std::move(r);
	return id;
}

JobId Jobs::retry(JobId id, JobSpec spec) {
	if (!started_ || stopping_) return 0;
	return q_.retry(id, std::move(spec));
}

JobId Jobs::queueCleanup(const std::string& root, const std::string& device) {
	JobSpec s;
	s.name = "Cleanup";
	s.targetRoot = root;
	s.device = device;
	return add(JobKind::Cleanup, std::move(s));
}

void Jobs::queueDevSleeps(int n, uint32_t ms, bool failLast, bool commit, const std::string& root) {
	for (int i = 1; i <= n; i++) {
		JobSpec s;
		s.slug = "devjob" + std::to_string(i);
		s.name = "Dev sleep job " + std::to_string(i);
		s.sleepMs = ms;
		s.sleepFail = failLast && i == n;
		s.sleepCommit = commit;
		s.targetRoot = root;
		add(JobKind::Sleep, std::move(s));
	}
}

void Jobs::startHead() {
	const JobEntry* n = q_.nextToStart();
	if (!n) return;
	const JobId id = n->id;
	const JobKind kind = n->kind;
	const JobSpec spec = n->spec;
	std::shared_ptr<ProgressCell> cell = n->cell;
	std::shared_ptr<Shared> sh = shared_;
	Runner runner;
	if (auto it = runners_.find(kind); it != runners_.end()) runner = it->second;
	if (auto it = jobRunners_.find(id); it != jobRunners_.end()) runner = std::move(it->second);
	// Overrides of jobs that left the line unstarted (cancelled while queued) are dropped here.
	for (auto it = jobRunners_.begin(); it != jobRunners_.end();)
		it = (it->first == id || !q_.find(it->first)) ? jobRunners_.erase(it) : std::next(it);
	const FileOps ops = cfg_.fileOps;
	const auto cleanFn = cleanFn_;
	if (!worker_.running()) {
		q_.markStarted(id);
		q_.finish(id, JobState::Failed, "The install worker is not running.");
		return;
	}
	auto task = [id, kind, spec, cell, sh, runner, ops, cleanFn] {
		TaskOutcome out;
		RunCtx ctx{*cell, id, [&](const std::string& root, int64_t dirId) {
			Lock l(sh->mutex);
			sh->claims.push_back({id, root, dirId});
		}, [&](const std::string& root, const std::string& device) {
			JobSpec cs;
			cs.name = "Cleanup";
			cs.targetRoot = root;
			cs.device = device;
			return cleanFn(cs, *cell);
		}};
		bool run = true;
		if (writesJournal(kind)) {
			// An interrupted job's journal must be recovered first: writeStartJournal would overwrite it.
			cell->setStage(Stage::Checking);
			const std::string why = journalBlocker(spec.targetRoot, spec.device, ops);
			if (!why.empty()) {
				out = TaskOutcome{JobState::Failed, why};
				run = false;
			}
		}
		if (run) {
			if (cell->cancelled()) out = TaskOutcome{JobState::Cancelled, {}};
			else if (!runner) out = TaskOutcome{JobState::Failed, "This is not available yet."};
			else {
				try {
					out = runner(spec, ctx);
				} catch (const std::exception& e) {
					LOG("job %u: the runner threw: %s", id, e.what());
					out = TaskOutcome{JobState::Failed, "Something went wrong."};
				} catch (...) {
					out = TaskOutcome{JobState::Failed, "Something went wrong."};
				}
			}
		}
		cell->markFinished(); // the result is decided: a cancel that arrives from now on is refused (JobQueue::cancel returns false)
		{
			Lock l(sh->mutex);
			sh->claims.erase(std::remove_if(sh->claims.begin(), sh->claims.end(), [id](const Shared::Claim& c) { return c.owner == id; }),
			                 sh->claims.end());
			sh->results.push_back({id, out.state, out.error});
		}
	};
	q_.markStarted(id);
	if (!worker_.submit(std::move(task))) q_.finish(id, JobState::Failed, "The install worker is not running.");
}

void Jobs::applyResults() {
	std::vector<Shared::Result> got;
	{
		Lock l(shared_->mutex);
		got.swap(shared_->results);
	}
	for (const Shared::Result& r : got) {
		const JobEntry* e = q_.find(r.id);
		const JobKind kind = e ? e->kind : JobKind::Sleep;
		const std::string name = e ? e->spec.name : std::string();
		const bool wasCancelled = e && e->state == JobState::Cancelled;
		q_.finish(r.id, r.state, r.error);
		const JobState final = wasCancelled ? JobState::Cancelled : r.state;
		// Internal jobs that go well stay quiet (a Cleanup that found nothing is not news).
		if (!(isInternal(kind) && final == JobState::Done))
			LOG("job %u [%s%s%s] %s%s%s", r.id, kindName(kind), name.empty() ? "" : " ", name.c_str(), stateName(final),
			    r.error.empty() || wasCancelled ? "" : ": ", wasCancelled ? "" : r.error.c_str());
		logged_.erase(r.id);
	}
}

JobView Jobs::view(const JobEntry& e) const {
	JobView v;
	v.id = e.id;
	v.kind = e.kind;
	v.state = e.state;
	v.slug = e.spec.slug;
	v.name = e.spec.name;
	v.version = e.spec.version;
	v.error = e.error;
	v.position = q_.position(e.id); // user jobs only; -1 for internal jobs and history entries (not in the line)
	v.internal = isInternal(e.kind);
	if (e.running) {
		v.stage = e.state == JobState::Cancelled ? "Cancelling" : stageLabel(e.cell->stage());
		v.done = e.cell->done();
		v.total = e.cell->total();
		v.inCommit = e.cell->inCommit();
		v.cancelling = e.state == JobState::Cancelled;
	}
	return v;
}

void Jobs::buildSnapshot() {
	snap_.line.clear();
	snap_.history.clear();
	for (const JobEntry& e : q_.line()) snap_.line.push_back(view(e));
	for (const JobEntry& e : q_.history()) snap_.history.push_back(view(e));
	snap_.active = q_.userActiveCount();
	snap_.next = q_.nextQueued();
	snap_.busy = q_.busy();
	snap_.seq++;
}

// Progress is logged from the main thread's snapshot only (never from the worker): every state change, and about once a second
// for the running job when logProgress is on.
void Jobs::logProgress() {
	const uint32_t now = cfg_.nowMs();
	for (const JobView& v : snap_.line) {
		if (v.internal) continue;
		auto it = logged_.find(v.id);
		if (it == logged_.end() || it->second != v.state) {
			logged_[v.id] = v.state;
			LOG("job %u [%s%s%s] %s%s%s position %d", v.id, kindName(v.kind), v.name.empty() ? "" : " ", v.name.c_str(), stateName(v.state),
			    v.stage[0] ? " / " : "", v.stage, v.position);
			lastLogMs_ = now;
		} else if (cfg_.logProgress && v.position == 0 && v.total && now - lastLogMs_ >= 1000) {
			LOG("job %u [%s%s%s] %s %llu/%llu (%d%%)%s", v.id, kindName(v.kind), v.name.empty() ? "" : " ", v.name.c_str(), v.stage, (unsigned long long)v.done,
			    (unsigned long long)v.total, (int)(v.done * 100 / v.total), v.inCommit ? " (committing)" : "");
			lastLogMs_ = now;
		}
	}
	if (logged_.size() > snap_.line.size() + 8) { // forget jobs that have left the line
		for (auto it = logged_.begin(); it != logged_.end();) {
			const bool inLine = std::any_of(snap_.line.begin(), snap_.line.end(), [&](const JobView& v) { return v.id == it->first; });
			it = inLine ? std::next(it) : logged_.erase(it);
		}
	}
}

void Jobs::update() {
	if (!started_ || stopping_) return;
	applyResults();
	q_.syncRunning();
	startHead();
	q_.syncRunning();
	buildSnapshot();
	logProgress();
}

void Jobs::beginShutdown() {
	if (!started_ || stopping_) return;
	stopping_ = true;
	shutdownT0_ = cfg_.nowMs();
	stuckAtShutdown_ = q_.cancelAll();
	if (stuckAtShutdown_) LOG("shutdown: a job is committing; waiting for it (up to %u s)", cfg_.commitWaitMs / 1000);
	worker_.requestStop();
}

void Jobs::shutdown() {
	if (!started_) return;
	beginShutdown();
	const uint32_t t0 = shutdownT0_;
	for (;;) {
		if (worker_.exited()) {
			worker_.join();
			if (cfg_.measureStack) LOG("perf: install worker stack: deepest use about %zu of %zu bytes (a lower bound)", worker_.stackHighWater(), worker_.stackBytes());
			LOG("install worker stopped in %u ms", cfg_.nowMs() - t0);
			break;
		}
		const JobEntry* r = q_.running();
		const bool committing = r && r->cell->inCommit();
		const uint32_t budget = committing ? cfg_.commitWaitMs : cfg_.exitWaitMs;
		if (cfg_.nowMs() - t0 > budget) {
			// A detached worker keeps running on its own copy of the state and may still LOG after the App has torn down logging
			// (debuglog::closeFile, SDL_Quit): at exit only, PC and DEBUGTOOLS builds, harmless to the card (the journal covers a cut).
			LOG("shutdown: the install worker did not stop within %u ms%s; leaving it (the journal makes a cut commit recoverable)", budget,
			    committing ? " (committing)" : "");
			worker_.detach();
			break;
		}
		SDL_Delay(5);
	}
	applyResults(); // settles the books (a no-op for a detached worker)
	started_ = false;
}

} // namespace install
