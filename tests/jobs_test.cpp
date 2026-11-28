// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the install worker, install::Jobs and the Cleanup job (M3 Increment 7), on real SDL threads (no SDL_Init) with
// short Sleep jobs and gated runners. The pure queue is in install_test.
#include <sys/stat.h>
#include <unistd.h>

#include <SDL_timer.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Check.h"
#include "install/AppsDevice.h"
#include "install/Commit.h"
#include "install/InstallWorker.h"
#include "install/Jobs.h"
#include "install/Journal.h"
#include "install/Recovery.h"

namespace fs = std::filesystem;
using namespace install;

struct Temp {
	std::string path;
	Temp() {
		char t[] = "/tmp/openshop-jobs-XXXXXX";
		path = mkdtemp(t);
	}
	~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
static void put(const std::string& p, const std::string& data) {
	std::error_code ec;
	fs::create_directories(fs::path(p).parent_path(), ec);
	std::ofstream(p, std::ios::binary) << data;
}
static bool there(const std::string& p) { std::error_code ec; return fs::symlink_status(p, ec).type() != fs::file_type::not_found; }
static uint32_t now() { return SDL_GetTicks(); }

// Calls update() until `pred` or the timeout; returns whether pred held. Each call must itself be quick (the main thread never waits).
static bool pump(Jobs& j, const std::function<bool()>& pred, uint32_t timeoutMs = 5000) {
	const uint32_t t0 = now();
	for (;;) {
		j.update();
		if (pred()) return true;
		if (now() - t0 > timeoutMs) return false;
		SDL_Delay(2);
	}
}
static bool allIdle(Jobs& j) { return j.queue().activeCount() == 0; }

static Jobs::Config cfgFor(const std::string& root) {
	Jobs::Config c;
	c.removeRoots = {root};
	return c;
}

static void testWorkerBasics() {
	InstallWorker w;
	CHECK(!w.submit([] {}) && !w.running());
	InstallWorker::Options o;
	o.measureStack = true;
	CHECK(w.start(o) && w.running() && !w.start(o));
	std::mutex m;
	std::vector<int> order;
	std::atomic<int> done{0};
	for (int i = 0; i < 20; i++)
		CHECK(w.submit([&, i] {
			std::lock_guard<std::mutex> l(m);
			order.push_back(i);
			done++;
		}));
	// a task that burns some stack, so the measurement has something to see
	std::atomic<size_t> sink{0};
	CHECK(w.submit([&] {
		volatile char buf[8 * 1024];
		for (size_t i = 0; i < sizeof buf; i += 64) buf[i] = (char)i;
		sink = buf[64];
		done++;
	}));
	const uint32_t t0 = now();
	while (done < 21 && now() - t0 < 3000) SDL_Delay(2);
	CHECK(done == 21);
	{
		std::lock_guard<std::mutex> l(m);
		bool inOrder = order.size() == 20;
		for (int i = 0; i < (int)order.size(); i++) inOrder = inOrder && order[i] == i;
		CHECK(inOrder);
	}
	w.requestStop();
	CHECK(!w.submit([] {}));
	w.join();
	CHECK(!w.running() && w.exited());
	CHECK(w.stackBytes() == 64 * 1024);
#ifdef __SANITIZE_ADDRESS__
	CHECK(w.stackHighWater() >= 2048 && w.stackHighWater() < 64 * 1024); // ASan puts big locals on its own fake stack
#else
	CHECK(w.stackHighWater() >= 8 * 1024 && w.stackHighWater() < 64 * 1024);
#endif
	std::printf("  install worker stack high-water: %zu of %zu bytes\n", w.stackHighWater(), w.stackBytes());
	// tasks that never started are dropped (and destroyed) at stop
	InstallWorker w2;
	CHECK(w2.start({}));
	std::atomic<bool> release{false}, ran2{false};
	w2.submit([&] { while (!release) SDL_Delay(1); });
	auto flag = std::make_shared<int>(0);
	w2.submit([&, flag] { ran2 = true; });
	w2.requestStop();
	release = true;
	w2.join();
	CHECK(!ran2 && flag.use_count() == 1);
	// the destructor stops and joins a running worker
	{
		InstallWorker w3;
		CHECK(w3.start({}));
	}
	// an exception in a task does not kill the thread
	InstallWorker w4;
	CHECK(w4.start({}));
	std::atomic<bool> after{false};
	w4.submit([] { throw 1; });
	w4.submit([&] { after = true; });
	const uint32_t t1 = now();
	while (!after && now() - t1 < 3000) SDL_Delay(2);
	CHECK(after);
}

// devjobs=3: strictly in sequence, history newest first, the last can fail.
static void testSequentialSleeps() {
	Temp t;
	Jobs j;
	CHECK(j.start(cfgFor(t.path)));
	std::atomic<int> concurrent{0}, maxConcurrent{0};
	std::mutex m;
	std::vector<JobId> started;
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx& ctx) {
		const int c = ++concurrent;
		int prev = maxConcurrent.load();
		while (c > prev && !maxConcurrent.compare_exchange_weak(prev, c)) {}
		{
			std::lock_guard<std::mutex> l(m);
			started.push_back(ctx.id);
		}
		SDL_Delay(30);
		--concurrent;
		return TaskOutcome{};
	});
	JobSpec s;
	s.slug = "x";
	const JobId a = j.add(JobKind::Install, s), b = j.add(JobKind::Install, s), c = j.add(JobKind::Install, s);
	CHECK(j.busy());
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(maxConcurrent == 1);
	CHECK((started == std::vector<JobId>{a, b, c}));
	CHECK(j.snapshot().history.size() == 3 && j.snapshot().history[0].id == c && j.snapshot().history[2].id == a);
	CHECK(!j.busy() && j.snapshot().active == 0 && j.snapshot().line.empty());
	// Sleep jobs, the last one fails (devjobfail=1)
	j.queueDevSleeps(3, 120, true, false, t.path);
	CHECK(j.busy());
	bool sawProgress = false, sawSecondQueued = false;
	CHECK(pump(j, [&] {
		const JobSnapshot& sn = j.snapshot();
		if (!sn.line.empty() && sn.line[0].total && sn.line[0].done && sn.line[0].done <= sn.line[0].total && std::string(sn.line[0].stage) == "Working") sawProgress = true;
		if (sn.line.size() == 3 && sn.line[1].state == JobState::Queued && sn.line[1].position == 1 && sn.next == sn.line[1].id) sawSecondQueued = true;
		return allIdle(j);
	}));
	CHECK(sawProgress && sawSecondQueued);
	const JobSnapshot& sn = j.snapshot();
	CHECK(sn.history.size() == 6);
	CHECK(sn.history[0].state == JobState::Failed && sn.history[0].error == "Dev job failed on purpose." && sn.history[1].state == JobState::Done &&
	      sn.history[2].state == JobState::Done);
	CHECK(sn.history[0].name == "Dev sleep job 3" && sn.history[0].position == -1);
	j.clearFinished();
	j.update();
	CHECK(j.snapshot().history.empty());
	j.shutdown();
}

static void testCancel() {
	Temp t;
	Jobs j;
	CHECK(j.start(cfgFor(t.path)));
	std::atomic<int> entered{0};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx& ctx) {
		entered++;
		ctx.cell.setStage(Stage::Downloading);
		while (!ctx.cell.cancelled()) SDL_Delay(1);
		return TaskOutcome{JobState::Cancelled, {}};
	});
	JobSpec s;
	const JobId a = j.add(JobKind::Install, s), b = j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return entered == 1 && j.snapshot().line.size() == 2 && j.snapshot().line[0].state == JobState::Downloading; }));
	CHECK(j.snapshot().line[0].id == a && j.snapshot().line[1].state == JobState::Queued);
	// cancelling a queued job removes it
	const JobId c = j.add(JobKind::Install, s);
	j.update();
	CHECK(j.snapshot().line.size() == 3);
	CHECK(j.cancel(c));
	j.update();
	CHECK(j.snapshot().line.size() == 2 && j.queue().find(c) == nullptr);
	// cancelling the running job reaches Cancelled within one chunk interval, and the next job starts
	const uint32_t t0 = now();
	CHECK(j.cancel(a));
	j.update();
	CHECK(j.snapshot().line[0].state == JobState::Cancelled && j.snapshot().line[0].cancelling && std::string(j.snapshot().line[0].stage) == "Cancelling");
	CHECK(j.snapshot().line.size() == 2 && j.snapshot().line[1].state == JobState::Queued); // b still waits for the worker to let go of a
	CHECK(pump(j, [&] { return entered == 2; }, 1000));
	CHECK(now() - t0 < 500);
	CHECK(j.queue().find(a) == nullptr && j.snapshot().history.empty()); // no history entry for a cancelled job
	CHECK(j.cancel(b));
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history.empty());
	j.shutdown();
}

// Quitting during a Sleep job returns within about 1 s; during a simulated commit it waits for it.
static void testShutdown() {
	Temp t;
	{
		Jobs j;
		CHECK(j.start(cfgFor(t.path)));
		j.queueDevSleeps(3, 30000, false, false, t.path);
		CHECK(pump(j, [&] { return !j.snapshot().line.empty() && j.snapshot().line[0].total > 0; }));
		const uint32_t t0 = now();
		j.shutdown();
		const uint32_t took = now() - t0;
		std::printf("  shutdown during a 30 s Sleep job: %u ms\n", took);
		CHECK(took < 1000);
		CHECK(!j.running() && j.queue().activeCount() == 0 && j.queue().history().empty());
		j.shutdown(); // twice is fine
	}
	{
		Jobs j;
		CHECK(j.start(cfgFor(t.path)));
		j.queueDevSleeps(2, 1000, false, true, t.path); // the second half (500 ms) is the commit phase
		CHECK(pump(j, [&] { return !j.snapshot().line.empty() && j.snapshot().line[0].inCommit; }));
		// cancelling a committing job is refused
		CHECK(!j.cancel(j.snapshot().line[0].id));
		const uint32_t t0 = now();
		j.shutdown();
		const uint32_t took = now() - t0;
		std::printf("  shutdown during a simulated commit: %u ms\n", took);
		CHECK(took >= 200); // it waited for the commit phase (about 500 ms in all)
		CHECK(!j.queue().history().empty() && j.queue().history()[0].state == JobState::Done); // the committing job finished
		CHECK(j.queue().activeCount() == 0); // the queued second job was cancelled
	}
	{
		// a commit that outlasts the budget is abandoned, and the worker's state stays alive for it
		Jobs j;
		Jobs::Config c = cfgFor(t.path);
		c.commitWaitMs = 100;
		CHECK(j.start(c));
		j.queueDevSleeps(1, 1200, false, true, t.path);
		CHECK(pump(j, [&] { return !j.snapshot().line.empty() && j.snapshot().line[0].inCommit; }));
		const uint32_t t0 = now();
		j.shutdown();
		CHECK(now() - t0 < 600);
		SDL_Delay(900); // the abandoned thread finishes on its own, after the Jobs object may be gone
	}
}

// A Refresh (or anything else the UI does) cannot touch the job system: the same ids finish, and update() never waits.
static void testMainThreadNeverBlocks() {
	Temp t;
	Jobs j;
	CHECK(j.start(cfgFor(t.path)));
	std::atomic<bool> release{false}, inside{false};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx&) {
		inside = true;
		while (!release) SDL_Delay(1);
		return TaskOutcome{};
	});
	JobSpec s;
	s.name = "n";
	const JobId a = j.add(JobKind::Install, s), b = j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return inside.load(); }));
	uint32_t worst = 0;
	for (int i = 0; i < 200; i++) {
		const uint32_t t0 = SDL_GetPerformanceCounter() ? now() : 0;
		j.update();
		worst = std::max(worst, now() - t0);
		SDL_Delay(1);
	}
	CHECK(worst < 50);
	CHECK(j.snapshot().line.size() == 2 && j.snapshot().line[0].id == a && j.snapshot().line[1].id == b);
	release = true;
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history.size() == 2 && j.snapshot().history[0].id == b && j.snapshot().history[1].id == a);
	j.shutdown();
}

static void testNoRunnerAndThrow() {
	Temp t;
	Jobs j;
	CHECK(j.start(cfgFor(t.path)));
	JobSpec s;
	s.targetRoot = t.path;
	s.device = "sd";
	j.add(JobKind::Update, s); // no runner for Update yet
	j.add(JobKind::Delete, s);
	j.setRunner(JobKind::Install, [](const JobSpec&, RunCtx&) -> TaskOutcome { throw 42; });
	j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history.size() == 3);
	CHECK(j.snapshot().history[2].state == JobState::Failed && j.snapshot().history[2].error == "This is not available yet.");
	CHECK(j.snapshot().history[0].state == JobState::Failed && j.snapshot().history[0].error == "Something went wrong.");
	j.shutdown();
}

// A job must not start on a root while a journal is present.
static void testJournalRefusal() {
	Temp t;
	const std::string root = t.path + "/sd";
	fs::create_directories(root);
	CHECK(journalBlocker(root, "sd").empty());
	Jobs j;
	CHECK(j.start(cfgFor(root)));
	std::atomic<int> ran{0};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx&) {
		ran++;
		return TaskOutcome{};
	});
	JobSpec s;
	s.targetRoot = root;
	s.device = "sd";
	// no journal: runs
	j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(ran == 1 && j.snapshot().history[0].state == JobState::Done);
	// a start-phase journal: refused, with the reason, and the journal is untouched
	CHECK(writeStartJournal(root, "sd", 3, JournalKind::Install, "foo"));
	const std::string jp = journalPath(root);
	const std::string restart = journalBlocker(root, "sd");
	CHECK(restart.find("Restart OpenShop") != std::string::npos); // a readable journal: restarting recovers it
	j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(ran == 1 && j.snapshot().history[0].state == JobState::Failed && !j.snapshot().history[0].error.empty());
	CHECK(there(jp));
	// a corrupt one, a foreign one (written for usb) and only a backup are refused too
	// (review M2: these are NOT the restart text, because restarting does not help; they say what to remove)
	put(jp, "{garbage");
	const std::string corrupt = journalBlocker(root, "sd");
	CHECK(!corrupt.empty() && corrupt.find("Restart OpenShop") == std::string::npos && corrupt != restart);
	CHECK(corrupt.find("can't read") != std::string::npos && corrupt.find("apps/OpenShop/installed/.journal") != std::string::npos &&
	      corrupt.find("other device") != std::string::npos);
	fs::remove(jp);
	CHECK(journalBlocker(root, "sd").empty());
	CHECK(writeStartJournal(root, "usb", 4, JournalKind::Install, "foo")); // written for the other device
	const std::string foreign = journalBlocker(root, "sd");
	CHECK(foreign == corrupt);
	CHECK(journalBlocker(root, "usb").find("Restart OpenShop") != std::string::npos); // readable on its own device
	fs::remove(jp);
	put(jp + ".bak", "x");
	const std::string bakOnly = journalBlocker(root, "sd");
	CHECK(!bakOnly.empty() && bakOnly.find("Restart OpenShop") == std::string::npos);
	fs::remove(jp + ".bak");
	// the check does not apply to Sleep or Cleanup jobs (they write no journal)
	CHECK(writeStartJournal(root, "sd", 3, JournalKind::Install, "foo"));
	j.queueDevSleeps(1, 20, false, false, root);
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history[0].state == JobState::Done);
	j.shutdown();
}

static std::string stg(const std::string& root, int id) { return stagingAbs(root, id); }

// The Cleanup job: removes every staging dir that no journal names; keeps the one a journal names; skips a root whose journal is bad.
static void testCleanupJob() {
	Temp t;
	const std::string sd = t.path + "/sd", usb = t.path + "/usb";
	put(stg(sd, 1) + "/old/apps/Foo/saves/a.sav", "finished job: the parked old tree");
	put(stg(sd, 2) + "/root/apps/foo/boot.dol", "failed extraction");
	put(stg(sd, 3) + "/manifest.json", "{}");
	put(sd + "/apps/OpenShop/staging/stray", "s");
	put(sd + "/apps/Foo/boot.dol", "app");
	put(sd + "/apps/OpenShop/installed/foo.json", "{}");
	CHECK(writeStartJournal(sd, "sd", 3, JournalKind::Install, "foo")); // names job 3
	put(stg(usb, 8) + "/old/x", "precious");
	put(usb + "/apps/OpenShop/installed/.journal", "{garbage"); // a root whose journal cannot be read
	put(stg(usb, 9) + "/junk", "junk");
	Jobs j;
	Jobs::Config c = cfgFor(sd);
	c.removeRoots = {sd, usb};
	CHECK(j.start(c));
	// the journal-named dir stays, a Failed recovery's staging is protected (see commit_test), the rest goes
	const JobId a = j.queueCleanup(sd, "sd");
	const JobId b = j.queueCleanup(usb, "usb");
	CHECK(a && b && !j.busy()); // Cleanup does not count as busy
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(!there(stg(sd, 1)) && !there(stg(sd, 2)) && !there(sd + "/apps/OpenShop/staging/stray"));
	CHECK(there(stg(sd, 3) + "/manifest.json")); // named by the journal
	CHECK(there(sd + "/apps/Foo/boot.dol") && there(sd + "/apps/OpenShop/installed/foo.json") && there(journalPath(sd)));
	CHECK(there(stg(usb, 8) + "/old/x") && there(stg(usb, 9) + "/junk")); // unreadable journal: nothing removed
	CHECK(j.snapshot().history.empty()); // internal jobs leave no history
	// once the journal is gone the rest can go
	CHECK(removeJournal(sd));
	j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(!there(stg(sd, 3)) && there(sd + "/apps/OpenShop/staging"));
	// a root with no staging folder at all, and a root that is not there: nothing to do, no failure
	j.queueCleanup(t.path + "/nowhere", "sd");
	CHECK(pump(j, [&] { return allIdle(j); }));
	j.shutdown();
}

// A gate on FileOps::remove (plain function pointers: no captures): the first removal blocks until released, so a test can act while a
// Cleanup job is inside a directory. An injected `remove` also selects removeTree's path walk.
static std::atomic<bool> gGateOn{false}, gInside{false}, gRelease{false};
static std::atomic<int> gRemoves{0};
static int gateRemove(const char* p) {
	if (gGateOn && !gInside.exchange(true))
		while (!gRelease) SDL_Delay(1);
	gRemoves++;
	return std::remove(p);
}
static void gateReset() {
	gGateOn = true;
	gInside = false;
	gRelease = false;
	gRemoves = 0;
}
static int filesUnder(const std::string& d) {
	int n = 0;
	std::error_code ec;
	for (fs::recursive_directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_regular_file(ec)) n++;
	return n;
}
static void makeStaging(const std::string& root, int first, int count) {
	for (int i = 0; i < count; i++)
		for (const char* f : {"a", "b", "c"}) put(stg(root, first + i) + "/" + f, "x");
}

// A cancelled Cleanup job stops: not just between entries, also inside a directory (removeTree's stop poll). Whatever is left is
// an ordinary smaller tree and the next Cleanup finishes it. (review M5, L11)
static void testCleanupCancel() {
	Temp t;
	const std::string sd = t.path + "/sd";
	Jobs::Config c = cfgFor(sd);
	c.fileOps.remove = &gateRemove;
	Jobs j;
	CHECK(j.start(c));
	// (1) cancel() of the running Cleanup, while it is inside the first dir
	makeStaging(sd, 100, 30);
	gateReset();
	const JobId a = j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return gInside.load(); }));
	CHECK(j.queue().running() && j.queue().running()->id == a && j.queue().running()->kind == JobKind::Cleanup);
	CHECK(j.cancel(a));
	gRelease = true;
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(gRemoves == 1); // the one removal that was in flight; the poll before the next entry said stop (mid-dir: 100 still has b and c)
	CHECK(filesUnder(sd + "/apps/OpenShop/staging") == 89 && there(stg(sd, 100) + "/b") && !there(stg(sd, 100) + "/a") && there(stg(sd, 129) + "/c"));
	CHECK(j.snapshot().history.empty());
	// the next Cleanup finishes the job
	j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(filesUnder(sd + "/apps/OpenShop/staging") == 0 && !there(stg(sd, 100)));
	// (2) a USER job added while the Cleanup runs asks it to stop (it is retried at the next start); the user job then runs
	makeStaging(sd, 200, 30);
	gateReset();
	std::atomic<int> ran{0};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx&) {
		ran++;
		return TaskOutcome{};
	});
	const JobId b = j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return gInside.load(); }));
	CHECK(j.queue().find(b)->state != JobState::Cancelled);
	JobSpec us;
	us.targetRoot = sd;
	us.device = "sd";
	const JobId u = j.add(JobKind::Install, us);
	CHECK(u != 0 && j.queue().find(b)->state == JobState::Cancelled); // Cleanup was asked to stop at once
	j.update();
	CHECK(j.snapshot().line.size() == 2 && j.snapshot().line[0].cancelling && j.snapshot().line[0].position == -1 && j.snapshot().line[1].id == u &&
	      j.snapshot().line[1].position == 0 && j.snapshot().next == u); // (M1) the user job is "next", not one behind the Cleanup
	CHECK(ran == 0); // it waits until the worker has let go of the Cleanup
	gRelease = true;
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(ran == 1 && gRemoves == 1 && filesUnder(sd + "/apps/OpenShop/staging") == 89);
	CHECK(j.snapshot().history.size() == 1 && j.snapshot().history[0].id == u && j.snapshot().history[0].state == JobState::Done);
	// a user job queued while the Cleanup is not yet running (it is queued behind another job) cancels nothing
	gateReset();
	gInside = true; // no more gate stops
	std::atomic<bool> hold{true}, in1{false};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx&) {
		in1 = true;
		while (hold) SDL_Delay(1);
		return TaskOutcome{};
	});
	j.add(JobKind::Install, us);
	const JobId cl = j.queueCleanup(sd, "sd");
	j.add(JobKind::Install, us);
	CHECK(pump(j, [&] { return in1.load(); }));
	CHECK(j.queue().find(cl)->state == JobState::Queued);
	// (M1) positions: user jobs only, the Cleanup in the middle is -1
	CHECK(j.snapshot().line.size() == 3 && j.snapshot().line[0].position == 0 && j.snapshot().line[1].position == -1 && j.snapshot().line[1].internal &&
	      j.snapshot().line[2].position == 1 && j.snapshot().next == j.snapshot().line[2].id && j.snapshot().active == 2);
	hold = false;
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(filesUnder(sd + "/apps/OpenShop/staging") == 0); // that Cleanup ran in full
	gGateOn = false;
	j.shutdown();
}

// Quitting while a Cleanup is inside a directory stops it too, and the books are settled.
static void testCleanupShutdown() {
	Temp t;
	const std::string sd = t.path + "/sd";
	Jobs::Config c = cfgFor(sd);
	c.fileOps.remove = &gateRemove;
	Jobs j;
	CHECK(j.start(c));
	makeStaging(sd, 300, 30);
	gateReset();
	j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return gInside.load(); }));
	std::thread rel([] { SDL_Delay(100); gRelease = true; });
	j.shutdown();
	rel.join();
	CHECK(j.queue().activeCount() == 0 && gRemoves == 1 && filesUnder(sd + "/apps/OpenShop/staging") == 89);
	gGateOn = false;
}

// Live jobs' staging dirs are claimed through RunCtx and are skipped by the Cleanup routine; claims are per root and end with the job.
// (review M4) The Install-kind runner holds a claim and runs the Cleanup routine ITSELF (ctx.cleanup: the code the Cleanup job runs)
// while it holds it; a Cleanup job queued meanwhile waits behind it (the Install job is the running head of the FIFO).
static void testClaims() {
	Temp t;
	const std::string sd = t.path + "/sd", usb = t.path + "/usb";
	put(stg(sd, 5) + "/root/a", "claimed: the live job's files");
	put(stg(sd, 5) + "/marker", "m");
	put(stg(sd, 6) + "/root/a", "abandoned");
	put(stg(usb, 5) + "/root/a", "same id on the other root: not claimed there");
	Jobs::Config c = cfgFor(sd);
	c.removeRoots = {sd, usb};
	Jobs j;
	CHECK(j.start(c));
	std::atomic<bool> claimed{false}, release{false}, checked{false};
	std::atomic<bool> keptDuring{false}, abandonedGoneDuring{false}, otherRootGoneDuring{false}, outcomeDone{false};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx& ctx) {
		ctx.claimStaging(sd, 5); // before the dir is touched
		claimed = true;
		const TaskOutcome a = ctx.cleanup(sd, "sd"); // the Cleanup routine, while the claim is held
		const TaskOutcome b = ctx.cleanup(usb, "usb"); // a claim on sd: does not protect usb:'s dir with the same id
		outcomeDone = a.state == JobState::Done && b.state == JobState::Done;
		keptDuring = there(stg(sd, 5) + "/root/a") && there(stg(sd, 5) + "/marker");
		abandonedGoneDuring = !there(stg(sd, 6));
		otherRootGoneDuring = !there(stg(usb, 5));
		checked = true;
		while (!release) SDL_Delay(1);
		return TaskOutcome{};
	});
	JobSpec s;
	s.targetRoot = sd;
	s.device = "sd";
	const JobId inst = j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return checked.load(); }));
	CHECK(outcomeDone && keptDuring && abandonedGoneDuring && otherRootGoneDuring);
	// a Cleanup job queued while the claim is held waits behind the Install job: it cannot run concurrently on the one worker
	const JobId cl = j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return j.queue().find(cl) != nullptr && j.snapshot().line.size() == 2; }));
	for (int i = 0; i < 20; i++) {
		j.update();
		SDL_Delay(2);
	}
	CHECK(j.queue().find(inst)->running && j.queue().find(cl)->state == JobState::Queued && there(stg(sd, 5) + "/marker"));
	// the claim ends with the job, then the queued Cleanup removes the dir
	release = true;
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(!there(stg(sd, 5)) && !there(stg(sd, 6)) && there(sd + "/apps/OpenShop/staging"));
	CHECK(j.snapshot().history.size() == 1 && j.snapshot().history[0].state == JobState::Done);
	// released claim, explicit: a second job claims a new dir, ends, and the next Cleanup removes that dir
	put(stg(sd, 7) + "/root/a", "x");
	std::atomic<bool> done2{false};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx& ctx) {
		ctx.claimStaging(sd, 7);
		done2 = true;
		return TaskOutcome{JobState::Failed, "boom"}; // a failed job releases its claim too
	});
	j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return allIdle(j); }) && done2);
	CHECK(there(stg(sd, 7))); // left behind by the failed job
	j.queueCleanup(sd, "sd");
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(!there(stg(sd, 7)));
	j.shutdown();
}

// A runner that has returned can no longer be cancelled: the task wrapper marks the cell finished. (review M6)
static void testCancelAfterReturn() {
	Temp t;
	Jobs j;
	CHECK(j.start(cfgFor(t.path)));
	j.setRunner(JobKind::Install, [](const JobSpec&, RunCtx&) { return TaskOutcome{}; }); // Done at once
	JobSpec s;
	const JobId id = j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return j.queue().running() != nullptr; }));
	const std::shared_ptr<ProgressCell> cell = j.queue().running()->cell;
	const uint32_t t0 = now();
	while (!cell->finished() && now() - t0 < 3000) SDL_Delay(1); // no update(): the result is not applied yet
	CHECK(cell->finished());
	CHECK(j.queue().running() && j.queue().running()->id == id && j.queue().running()->state != JobState::Cancelled);
	CHECK(!j.cancel(id)); // refused
	CHECK(j.queue().find(id)->state != JobState::Cancelled);
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history.size() == 1 && j.snapshot().history[0].state == JobState::Done); // the runner's own result stands
	// a runner that is still running can be cancelled as before
	std::atomic<bool> in{false};
	j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx& ctx) {
		in = true;
		while (!ctx.cell.cancelled()) SDL_Delay(1);
		return TaskOutcome{JobState::Cancelled, {}};
	});
	const JobId b = j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return in.load(); }));
	CHECK(j.cancel(b));
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history.size() == 1);
	// a Failed result stands too
	j.setRunner(JobKind::Install, [](const JobSpec&, RunCtx&) { return TaskOutcome{JobState::Failed, "nope"}; });
	const JobId f = j.add(JobKind::Install, s);
	CHECK(pump(j, [&] { return j.queue().running() != nullptr; }));
	const std::shared_ptr<ProgressCell> fc = j.queue().running()->cell;
	const uint32_t t1 = now();
	while (!fc->finished() && now() - t1 < 3000) SDL_Delay(1);
	CHECK(!j.cancel(f));
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(j.snapshot().history[0].state == JobState::Failed && j.snapshot().history[0].error == "nope");
	j.shutdown();
}

// Cancel and "enter commit" exclude each other: with both called at the same moment from two threads, exactly one wins, every time.
static void testCancelCommitRace() {
	constexpr int kN = 50000;
	auto cells = std::make_unique<ProgressCell[]>(kN);
	auto arrived = std::make_unique<std::atomic<int>[]>(kN);
	auto cancelWon = std::make_unique<std::atomic<int>[]>(kN);
	auto commitWon = std::make_unique<std::atomic<int>[]>(kN);
	for (int i = 0; i < kN; i++) arrived[i] = cancelWon[i] = commitWon[i] = 0;
	auto meet = [&](int i) { // both threads leave together
		arrived[i].fetch_add(1);
		while (arrived[i].load() < 2) {}
	};
	std::thread a([&] {
		for (int i = 0; i < kN; i++) {
			meet(i);
			cancelWon[i] = cells[i].requestCancel() ? 1 : 0;
		}
	});
	std::thread b([&] {
		for (int i = 0; i < kN; i++) {
			meet(i);
			commitWon[i] = cells[i].enterCommit() ? 1 : 0;
		}
	});
	a.join();
	b.join();
	int cancels = 0, commits = 0, both = 0, neither = 0, inconsistent = 0;
	for (int i = 0; i < kN; i++) {
		const bool cw = cancelWon[i], mw = commitWon[i];
		cancels += cw && !mw;
		commits += mw && !cw;
		both += cw && mw;
		neither += !cw && !mw;
		if (cells[i].cancelled() != cw || cells[i].inCommit() != mw) inconsistent++;
	}
	std::printf("  cancel vs enterCommit over %d rounds: cancel won %d, commit won %d\n", kN, cancels, commits);
	CHECK(both == 0 && neither == 0 && inconsistent == 0 && cancels + commits == kN);
}

// add() is refused (0) when the job system is not running; busy() cannot be stuck. (review M3)
static void testNotRunning() {
	Temp t;
	JobSpec s;
	s.targetRoot = t.path;
	s.device = "sd";
	{
		Jobs j; // never started
		CHECK(!j.running() && j.add(JobKind::Sleep, s) == 0 && j.add(JobKind::Install, s) == 0 && j.queueCleanup(t.path, "sd") == 0);
		CHECK(!j.busy() && j.retry(1, s) == 0 && j.queue().activeCount() == 0);
		j.queueDevSleeps(3, 10, false, false, t.path);
		CHECK(!j.busy() && j.queue().activeCount() == 0);
		j.update(); // harmless
		j.shutdown();
	}
	{
		Jobs j;
		CHECK(j.start(cfgFor(t.path)));
		CHECK(j.add(JobKind::Sleep, s) != 0 && j.busy());
		j.beginShutdown(); // phase 1: new jobs are refused at once, queued ones are gone
		CHECK(j.add(JobKind::Sleep, s) == 0 && j.addWithRunner(JobKind::Sleep, s, [](const JobSpec&, RunCtx&) { return TaskOutcome{}; }) == 0);
		CHECK(!j.busy() && j.retry(1, s) == 0);
		j.beginShutdown(); // idempotent
		j.shutdown();
		CHECK(!j.running() && j.add(JobKind::Sleep, s) == 0 && !j.busy() && j.queue().activeCount() == 0);
	}
	{
		// an abandoned (detached) worker leaves its entry in the line, and busy() still answers false
		Jobs j;
		Jobs::Config c = cfgFor(t.path);
		c.commitWaitMs = 50;
		CHECK(j.start(c));
		j.queueDevSleeps(1, 800, false, true, t.path);
		CHECK(pump(j, [&] { return !j.snapshot().line.empty() && j.snapshot().line[0].inCommit; }));
		j.shutdown();
		CHECK(!j.busy() && j.add(JobKind::Sleep, s) == 0);
		SDL_Delay(900);
	}
	{
		// the runner override runs only its own job; a cancelled-before-start job drops its override
		Jobs j;
		CHECK(j.start(cfgFor(t.path)));
		std::atomic<int> own{0}, plain{0};
		j.setRunner(JobKind::Install, [&](const JobSpec&, RunCtx&) {
			plain++;
			return TaskOutcome{};
		});
		const JobId a = j.addWithRunner(JobKind::Install, s, [&](const JobSpec&, RunCtx&) {
			own++;
			return TaskOutcome{};
		});
		const JobId b = j.add(JobKind::Install, s);
		const JobId c = j.addWithRunner(JobKind::Install, s, [&](const JobSpec&, RunCtx&) {
			own += 100;
			return TaskOutcome{};
		});
		CHECK(a && b && c && j.cancel(c)); // never started
		CHECK(pump(j, [&] { return allIdle(j); }));
		CHECK(own == 1 && plain == 1);
	}
}

// setAppsDevice is refused while a dev Sleep job is queued or running; a Cleanup job does not block it.
static void testBusyGuard() {
	Temp t;
	const std::string sd = t.path + "/sd", usb = t.path + "/usb";
	fs::create_directories(sd);
	fs::create_directories(usb);
	Jobs j;
	CHECK(j.start(cfgFor(sd)));
	AppsDeviceSetting s;
	AppsDeviceSetting::Mounts m;
	m.sd = m.usb = true;
	m.sdRoot = sd;
	m.usbRoot = usb;
	s.init(t.path + "/appsdevice.ini", m);
	s.setBusy([&j] { return j.busy(); });
	CHECK(s.resolved() == Device::Sd);
	j.queueCleanup(sd, "sd");
	std::string why;
	CHECK(s.set(Device::Usb, &why) == AppsDeviceSetting::SetResult::Switched && s.resolved() == Device::Usb); // Cleanup: not busy
	CHECK(s.set(Device::Sd) == AppsDeviceSetting::SetResult::Switched);
	j.queueDevSleeps(1, 400, false, false, sd);
	CHECK(s.set(Device::Usb, &why) == AppsDeviceSetting::SetResult::Refused && !why.empty() && s.resolved() == Device::Sd); // queued
	CHECK(pump(j, [&] { return !j.snapshot().line.empty() && j.snapshot().line[0].kind == JobKind::Sleep && j.snapshot().line[0].total > 0; }));
	CHECK(s.set(Device::Usb, &why) == AppsDeviceSetting::SetResult::Refused && s.resolved() == Device::Sd); // running
	CHECK(pump(j, [&] { return allIdle(j); }));
	CHECK(s.set(Device::Usb) == AppsDeviceSetting::SetResult::Switched && s.resolved() == Device::Usb);
	j.shutdown();
}

int main() {
	testWorkerBasics();
	testSequentialSleeps();
	testCancel();
	testShutdown();
	testMainThreadNeverBlocks();
	testNoRunnerAndThrow();
	testJournalRefusal();
	testCleanupJob();
	testCleanupCancel();
	testCleanupShutdown();
	testClaims();
	testCancelAfterReturn();
	testCancelCommitRace();
	testNotRunning();
	testBusyGuard();
	return checkReport("jobs_test");
}
