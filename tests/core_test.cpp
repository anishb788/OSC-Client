// Host tests for small pure pieces outside the catalog: dev option parsing, the catalog install guard and the Refresh rate limit.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "catalog/InstallGuard.h"
#include "catalog/LoadTracker.h"
#include "catalog/RefreshPolicy.h"
#include "core/DevOptions.h"
#include "core/PathRoot.h"
#include "core/StackProbe.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static DevOptions parse(const std::string& text) {
	DevOptions o;
	DevOptions::parseFile(text, o);
	return o;
}

static void testDevOptions() {
	DevOptions o = parse("screen=detail\nmock=1\nfail=1\n");
	CHECK(o.screen == "detail" && o.mock && o.failStartup && !o.failRefresh && !o.offline && !o.refresh);

	// fail=: 1 / startup / any other truthy value mean startup; refresh; both; the rest is off.
	CHECK(parse("fail=startup\n").failStartup && !parse("fail=startup\n").failRefresh);
	CHECK(parse("fail=refresh\n").failRefresh && !parse("fail=refresh\n").failStartup);
	CHECK(parse("fail=both\n").failStartup && parse("fail=both\n").failRefresh);
	CHECK(parse("fail=yes\n").failStartup && !parse("fail=yes\n").failRefresh);
	for (const char* off : {"fail=0\n", "fail=false\n", "fail=off\n", "fail=\n", "fail=none\n", ""}) {
		CHECK(!parse(off).failStartup && !parse(off).failRefresh);
	}
	CHECK(!parse("fail=both\nfail=0\n").failRefresh); // last value wins, fully
	CHECK(!parse("fail=refresh\nfail=startup\n").failRefresh);

	// api=: the catalog API base, without trailing slashes; empty = the real server.
	CHECK(parse("").apiBase.empty());
	CHECK(parse("api=http://127.0.0.1:9/api/v4/\n").apiBase == "http://127.0.0.1:9/api/v4");
	CHECK(parse("api=http://h/x?a=b\n").apiBase == "http://h/x?a=b");
	CHECK(parse("api=http://h/v4\n").describe().find(" api=http://h/v4") != std::string::npos);
	// icons=: where icons are fetched from, "<base>/<slug>/icon.png"; empty = the catalog's URLs.
	CHECK(parse("").iconBase.empty());
	CHECK(parse("icons=http://127.0.0.1:9/icons/\n").iconBase == "http://127.0.0.1:9/icons");
	CHECK(parse("icons=http://h/i\n").describe().find(" icons=http://h/i") != std::string::npos);
	CHECK(parse("").autoRefreshMs == -1 && parse("autorefresh=4000\n").autoRefreshMs == 4000 && parse("autorefresh=x\n").autoRefreshMs == -1);
	// mockdelay= / ratelimit=: non-negative integers; anything else keeps the built-in default (-1).
	CHECK(parse("").mockDelayMs == -1 && parse("").rateLimitSec == -1);
	CHECK(parse("mockdelay=4000\n").mockDelayMs == 4000);
	CHECK(parse("mockdelay=0\n").mockDelayMs == 0);
	CHECK(parse("mockdelay=abc\n").mockDelayMs == -1);
	CHECK(parse("mockdelay=-5\n").mockDelayMs == -1);
	CHECK(parse("ratelimit=0\n").rateLimitSec == 0);
	CHECK(parse("ratelimit=15\n").rateLimitSec == 15);
	CHECK(parse("ratelimit=\n").rateLimitSec == -1);
	// cachettl=: the catalog cache TTL in seconds (0 = always stale); anything else keeps the built-in 1 h (-1).
	CHECK(parse("").cacheTtlSec == -1 && parse("cachettl=0\n").cacheTtlSec == 0 && parse("cachettl=90\n").cacheTtlSec == 90);
	CHECK(parse("cachettl=x\n").cacheTtlSec == -1 && parse("cachettl=-1\n").cacheTtlSec == -1);
	CHECK(parse("cachettl=90\n").describe().find(" cachettl=90") != std::string::npos && parse("").describe().find("cachettl") == std::string::npos);

	// CRLF, spaces, comments, unknown keys, bare screen name (old format), last value wins.
	o = parse("# comment\r\n  screen = list \r\nunknown=1\r\noffline=1\r\nrefresh=1\r\n");
	CHECK(o.screen == "list" && o.offline && o.refresh && !o.failStartup);
	CHECK(parse("search").screen == "search");
	CHECK(parse("screen=home\nscreen=dialog\n").screen == "dialog");
	CHECK(parse("").screen.empty());

	// topbar=all previews the top-bar buttons; anything else leaves them to their features.
	CHECK(!parse("").topbarAll);
	CHECK(parse("topbar=all\n").topbarAll);
	CHECK(parse(" topbar = all \r\n").topbarAll);
	CHECK(!parse("topbar=none\n").topbarAll);
	CHECK(!parse("topbar=1\n").topbarAll);
	CHECK(!parse("topbar=\n").topbarAll);
	CHECK(!parse("topbar=all\ntopbar=\n").topbarAll);
	CHECK(parse("topbar=all\nscreen=list\n").screen == "list");

	o = parse("screen=search\nfail=1");
	CHECK(o.describe() == "screen=search mock=0 offline=0 refresh=0 fail=startup topbar=default mockdelay=-1 ratelimit=-1 perf=0");
	// active(): empty for a normal launch (the default screen "home" included), otherwise only what was changed.
	CHECK(parse("").active().empty() && parse("screen=home\n").active().empty());
	CHECK(parse("mock=1\nfail=both\nratelimit=0\n").active() == "mock=1 fail=both ratelimit=0");
	CHECK(parse("screen=detail\ncachettl=0\n").active() == "screen=detail cachettl=0");
	CHECK(parse("perf=1").describe().find("perf=1") != std::string::npos);
	CHECK(parse("fail=both\nmockdelay=900\n").describe().find("fail=both") != std::string::npos);
	CHECK(parse("fail=refresh").describe().find("fail=refresh") != std::string::npos);

	// The network self-test is off unless asked for, and does not show in describe() then.
	o = parse("screen=home");
	CHECK(o.nettest.empty() && o.nettestBg.empty() && !o.nettestQuit);
	o = parse("nettest=http://127.0.0.1:8099/len/10,http://x/y\nnettestbg=http://h/big\nnettestquit=1\n");
	CHECK(o.nettest == "http://127.0.0.1:8099/len/10,http://x/y");
	CHECK(o.nettestBg == "http://h/big");
	CHECK(o.nettestQuit);
	CHECK(o.describe().find("nettestquit=1") != std::string::npos);

	// describe() with only nettestbg set still shows the group (with an empty nettest and quit off).
	o = parse("nettestbg=http://h/big\n");
	CHECK(o.describe().find("nettest= nettestbg=http://h/big nettestquit=0") != std::string::npos);
	CHECK(parse("nettest=http://h/x\n").describe().find("nettest=http://h/x nettestbg= nettestquit=0") != std::string::npos);
}

// The environment path (PC): OPENSHOP_NETTEST / NETTESTBG / NETTESTQUIT, and what wins over devstart.
static void testNetTestEnv() {
	const char* names[] = {"OPENSHOP_NETTEST", "OPENSHOP_NETTESTBG", "OPENSHOP_NETTESTQUIT"};
	for (const char* n : names) unsetenv(n);
	const std::string noDir = "/nonexistent-openshop-dir";

	DevOptions o = DevOptions::load(noDir);
	CHECK(o.nettest.empty() && o.nettestBg.empty() && !o.nettestQuit);

	setenv("OPENSHOP_NETTEST", "http://h/a,waitbg", 1);
	setenv("OPENSHOP_NETTESTBG", "http://h/big", 1);
	setenv("OPENSHOP_NETTESTQUIT", "1", 1);
	o = DevOptions::load(noDir);
	CHECK(o.nettest == "http://h/a,waitbg" && o.nettestBg == "http://h/big" && o.nettestQuit);

	// nettestquit=0 means off (not "any non-empty value"), and an empty variable is ignored.
	setenv("OPENSHOP_NETTESTQUIT", "0", 1);
	CHECK(!DevOptions::load(noDir).nettestQuit);
	setenv("OPENSHOP_NETTESTQUIT", "off", 1);
	CHECK(!DevOptions::load(noDir).nettestQuit);
	setenv("OPENSHOP_NETTESTQUIT", "", 1);
	CHECK(!DevOptions::load(noDir).nettestQuit);
	setenv("OPENSHOP_NETTEST", "", 1);
	CHECK(DevOptions::load(noDir).nettest.empty());

	// Only nettestbg set: describe() reports it.
	unsetenv("OPENSHOP_NETTEST");
	unsetenv("OPENSHOP_NETTESTQUIT");
	o = DevOptions::load(noDir);
	CHECK(o.nettest.empty() && o.nettestBg == "http://h/big");
	CHECK(o.describe().find("nettestbg=http://h/big") != std::string::npos);

	for (const char* n : names) unsetenv(n);
}

static void testInstallGuard() {
	// The recorded LoadingScreen alone, nothing pending, current loader: allowed, any number of times.
	InstallState ok;
	CHECK(catalogInstallRefusal(ok) == nullptr);
	CHECK(catalogInstallRefusal(ok) == nullptr); // a second install (the Refresh LoadingScreen) is fine

	// A second install while Home (or any other non-loading screen) exists is refused.
	InstallState otherScreen;
	otherScreen.onlyLoadingScreen = false;
	CHECK(catalogInstallRefusal(otherScreen) != nullptr);

	// A pending push, pop or reset refuses (a pending reset could put a screen on top).
	InstallState pending;
	pending.screenChangePending = true;
	CHECK(catalogInstallRefusal(pending) != nullptr);

	// A stale loader (replaced by a Retry, or from another screen) is refused.
	InstallState stale;
	stale.loaderMatches = false;
	CHECK(catalogInstallRefusal(stale) != nullptr);

	// Every condition is checked, and each refusal has a reason.
	InstallState all;
	all.onlyLoadingScreen = all.loaderMatches = false;
	all.screenChangePending = true;
	CHECK(catalogInstallRefusal(all) != nullptr);
}

static void testRefreshRateLimit() {
	CHECK(kRefreshMinIntervalSec == 60);
	CHECK(!refreshRateLimited(-1, 60));        // no fetch yet
	CHECK(refreshRateLimited(0, 60));
	CHECK(refreshRateLimited(59999, 60));
	CHECK(!refreshRateLimited(60000, 60));     // exactly 60 s ago: fetch
	CHECK(!refreshRateLimited(3600 * 1000, 60));
	CHECK(!refreshRateLimited(1000, 0));       // ratelimit=0 turns it off
	CHECK(refreshRateLimited(5000, 15) == true && refreshRateLimited(15000, 15) == false);
}

// The App wiring of the install guard, with fake screen pointers (only compared, never dereferenced).
static void testLoadTracker() {
	int a = 0, b = 0, r1 = 0, r2 = 0, home = 0; // distinct addresses standing in for screens
	const void *A = &a, *B = &b, *R1 = &r1, *R2 = &r2, *HOME = &home;

	// Startup: the recorded screen installs with its current token.
	LoadTracker t;
	CHECK(!t.installed() && t.generation() == 0 && t.msSinceFetch(1000) == -1);
	t.recordStartupScreen(A);
	uint32_t tok = t.beginLoad(A);
	CHECK(t.refusal(A, false, tok) == nullptr);
	CHECK(t.refusal(nullptr, false, tok) != nullptr); // more than one screen on the stack
	CHECK(t.refusal(B, false, tok) != nullptr);       // some other single screen
	CHECK(t.refusal(A, true, tok) != nullptr);        // a screen change is pending
	CHECK(t.refusal(A, false, 0) != nullptr);         // no token
	t.accepted(1000, true);
	CHECK(t.installed() && t.generation() == 1 && t.msSinceFetch(1500) == 500);

	// Retry: the replaced loader's token no longer installs.
	LoadTracker s;
	s.recordStartupScreen(A);
	uint32_t old = s.beginLoad(A), now = s.beginLoad(A);
	CHECK(s.refusal(A, false, old) != nullptr && s.refusal(A, false, now) == nullptr);

	// Refresh: the pending screen becomes the recorded one only when the reset swap makes it the only screen.
	t.setPendingScreen(R1);
	CHECK(t.recordedScreen() == A);                 // nothing swapped yet; old screens still alive
	CHECK(t.refusal(nullptr, true, tok) != nullptr);
	t.onStackReset(R1);
	CHECK(t.recordedScreen() == R1);
	CHECK(t.refusal(R1, false, tok) != nullptr);    // the startup token is stale for the new screen
	uint32_t rt = t.beginLoad(R1);
	CHECK(t.refusal(R1, false, rt) == nullptr);
	t.accepted(70000, true);
	CHECK(t.generation() == 2 && t.msSinceFetch(70000) == 0);

	// A reset that is not a refresh (to Home) leaves no recorded screen: nothing can install.
	t.onStackReset(HOME);
	CHECK(t.recordedScreen() == nullptr && t.refusal(HOME, false, rt) != nullptr);

	// A pending screen that never got its swap (the reset went elsewhere) is not recorded later by accident.
	t.setPendingScreen(R2);
	t.onStackReset(HOME);
	CHECK(t.recordedScreen() == nullptr);

	// A load started by some other screen invalidates the recorded screen's token.
	LoadTracker u;
	u.recordStartupScreen(A);
	uint32_t ut = u.beginLoad(A);
	(void)u.beginLoad(B);
	CHECK(u.refusal(A, false, ut) != nullptr);

	// A catalog served from the SD cache is installed but is not a fetch: the rate-limit clock doesn't move.
	LoadTracker c;
	c.recordStartupScreen(A);
	c.accepted(5000, false);
	CHECK(c.installed() && c.msSinceFetch(6000) == -1);
	CHECK(!refreshRateLimited(c.msSinceFetch(6000), kRefreshMinIntervalSec)); // first Refresh really fetches
	c.accepted(10000, true);
	c.accepted(20000, false);                       // a later cache install keeps the fetch time
	CHECK(c.msSinceFetch(30000) == 20000);
	CHECK(refreshRateLimited(c.msSinceFetch(30000), kRefreshMinIntervalSec));

	// The millisecond clock wraps (SDL ticks are 32-bit).
	LoadTracker w;
	w.accepted(0xFFFFFF00u, true);
	CHECK(w.msSinceFetch(0x100u) == 0x200);

	// Ten refreshes in a row: generation counts every accepted install.
	LoadTracker g;
	g.recordStartupScreen(A);
	g.accepted(0, true);
	int screens[10];
	for (int i = 0; i < 10; i++) {
		g.setPendingScreen(&screens[i]);
		g.onStackReset(&screens[i]);
		uint32_t k = g.beginLoad(&screens[i]);
		CHECK(g.refusal(&screens[i], false, k) == nullptr);
		g.accepted((uint32_t)(i + 1) * 1000, true);
	}
	CHECK(g.generation() == 11);
}

// heapprobe / stackprobe: off by default, set from devstart or the environment, silent in active() when off.
static void testProbeOptions() {
	for (const char* n : {"OPENSHOP_HEAPPROBE", "OPENSHOP_STACKPROBE"}) unsetenv(n);
	const std::string noDir = "/nonexistent-openshop-dir";
	DevOptions o = DevOptions::load(noDir);
	CHECK(!o.heapProbe && !o.stackProbe);
	CHECK(o.active().find("probe") == std::string::npos && o.describe().find("probe") == std::string::npos);
	o = parse("heapprobe=1\nstackprobe=1\n");
	CHECK(o.heapProbe && o.stackProbe);
	CHECK(o.active() == "heapprobe=1 stackprobe=1");
	CHECK(o.describe().find("heapprobe=1") != std::string::npos && o.describe().find("stackprobe=1") != std::string::npos);
	o = parse("heapprobe=0\nstackprobe=off\n");
	CHECK(!o.heapProbe && !o.stackProbe);
	setenv("OPENSHOP_HEAPPROBE", "1", 1);
	setenv("OPENSHOP_STACKPROBE", "0", 1);
	o = DevOptions::load(noDir);
	CHECK(o.heapProbe && !o.stackProbe);
	setenv("OPENSHOP_HEAPPROBE", "", 1); // empty = ignored
	CHECK(!DevOptions::load(noDir).heapProbe);
	for (const char* n : {"OPENSHOP_HEAPPROBE", "OPENSHOP_STACKPROBE"}) unsetenv(n);
}

static void testStackProbe() {
	unsigned char region[1000];
	stackprobe::paint(region, sizeof region);
	CHECK(stackprobe::highWater(region, sizeof region) == 0);
	// The stack grows down: writes at the top (high addresses) of the region count as shallow use.
	for (size_t i = 900; i < 1000; i++) region[i] = 0;
	CHECK(stackprobe::highWater(region, sizeof region) == 100);
	for (size_t i = 300; i < 350; i++) region[i] = 1;
	CHECK(stackprobe::highWater(region, sizeof region) == 700); // deepest write at offset 300
	region[0] = 0;
	CHECK(stackprobe::highWater(region, sizeof region) == 1000); // the whole region
	CHECK(stackprobe::highWater(region, 0) == 0);
}

static void testProbeExclusive() {
	using PC = DevOptions::ProbeChoice;
	CHECK(parse("").probeChoice() == PC::None);
	CHECK(parse("heapprobe=1\n").probeChoice() == PC::Heap);
	CHECK(parse("stackprobe=1\n").probeChoice() == PC::Stack);
	CHECK(parse("heapprobe=1\nstackprobe=1\n").probeChoice() == PC::Conflict);
	CHECK(parse("heapprobe=1\nstackprobe=0\n").probeChoice() == PC::Heap);
	// the commit probe (M3 Increment 6): exclusive with the other two, keys and environment, sizes
	CHECK(parse("commitprobe=cap\n").probeChoice() == PC::Commit && parse("commitprobe=flush\n").commitProbe == "flush");
	CHECK(parse("commitprobe=cap\nheapprobe=1\n").probeChoice() == PC::Conflict);
	CHECK(parse("commitprobe=cap\nstackprobe=1\n").probeChoice() == PC::Conflict);
	CHECK(parse("commitprobe=cap\nheapprobe=0\n").probeChoice() == PC::Commit);
	DevOptions o = parse("commitprobe=cap\ncommitprobe_files=19000\ncommitprobe_stack=32\n");
	CHECK(o.commitProbeFiles == 19000 && o.commitProbeStackKb == 32);
	CHECK(o.active() == "commitprobe=cap commitprobe_files=19000 commitprobe_stack=32");
	CHECK(parse("").commitProbe.empty() && parse("").commitProbeFiles < 0 && parse("").commitProbeStackKb < 0);
	CHECK(parse("").active().find("commitprobe") == std::string::npos && parse("").describe().find("commitprobe") == std::string::npos);
	setenv("OPENSHOP_COMMITPROBE", "flush", 1);
	setenv("OPENSHOP_COMMITPROBE_FILES", "123", 1);
	o = DevOptions::load("/nonexistent-openshop-dir");
	CHECK(o.commitProbe == "flush" && o.commitProbeFiles == 123 && o.probeChoice() == PC::Commit);
	for (const char* n : {"OPENSHOP_COMMITPROBE", "OPENSHOP_COMMITPROBE_FILES", "OPENSHOP_COMMITPROBE_STACK"}) unsetenv(n);
	// the claim probe (M3 Increment 7): a key and an environment variable, exclusive with the other probes, silent when off
	CHECK(parse("claimprobe=1\n").probeChoice() == PC::Claim && parse("claimprobe=1\n").active() == "claimprobe=1");
	CHECK(parse("claimprobe=0\n").probeChoice() == PC::None && parse("").active().find("claimprobe") == std::string::npos);
	CHECK(parse("claimprobe=1\nheapprobe=1\n").probeChoice() == PC::Conflict);
	CHECK(parse("claimprobe=1\nstackprobe=1\n").probeChoice() == PC::Conflict);
	CHECK(parse("claimprobe=1\ncommitprobe=cap\n").probeChoice() == PC::Conflict);
	CHECK(parse("claimprobe=1\n").describe().find("claimprobe=1") != std::string::npos);
	setenv("OPENSHOP_CLAIMPROBE", "1", 1);
	CHECK(DevOptions::load("/nonexistent-openshop-dir").probeChoice() == PC::Claim);
	setenv("OPENSHOP_CLAIMPROBE", "0", 1);
	CHECK(DevOptions::load("/nonexistent-openshop-dir").probeChoice() == PC::None);
	unsetenv("OPENSHOP_CLAIMPROBE");
}

static void testRootOf() {
	// PC-style roots, cwd /home/u/proj: relative and absolute spellings are the same place.
	const std::vector<std::string> pc = {"./sdcard", "./usbdrive"};
	const std::string cwd = "/home/u/proj";
	auto idx = [&](const std::string& p) { return pathroot::indexOf(pc, p, cwd); };
	CHECK(idx("./sdcard") == 0);
	CHECK(idx("sdcard") == 0);
	CHECK(idx("sdcard/") == 0);
	CHECK(idx("./sdcard/apps/OpenShop") == 0);
	CHECK(idx("/home/u/proj/sdcard/apps/x") == 0); // absolute path, relative root
	CHECK(idx("/home/u/proj/./sdcard//apps/") == 0);
	CHECK(idx("./usbdrive/x") == 1);
	CHECK(idx("sdcard/apps/../apps/y") == 0);
	CHECK(idx("sdcard/../x") == -1);               // .. leaves the root
	CHECK(idx("sdcard/../usbdrive/x") == 1);       // and lands on another one
	CHECK(idx("./sdcard2") == -1);                 // prefix, not a component
	CHECK(idx("./sdcard2/apps") == -1);
	CHECK(idx("/home/u/proj/sdcard2") == -1);
	CHECK(idx("/elsewhere/sdcard") == -1);
	CHECK(idx("") == -1);
	CHECK(idx(".") == -1);                          // the cwd itself is above the roots
	CHECK(idx("/") == -1);
	// An absolute root (OPENSHOP_SD=/data/sd) against relative and absolute paths.
	const std::vector<std::string> absr = {"/data/sd"};
	CHECK(pathroot::indexOf(absr, "/data/sd/apps", "/home/u/proj") == 0);
	CHECK(pathroot::indexOf(absr, "/data/sd/", "/home/u/proj") == 0);
	CHECK(pathroot::indexOf(absr, "/data/sdx", "/home/u/proj") == -1);
	CHECK(pathroot::indexOf(absr, "/data/sd/../x", "/home/u/proj") == -1);
	CHECK(pathroot::indexOf(absr, "data/sd", "/home/u/proj") == -1); // relative to the cwd, a different place
	CHECK(pathroot::indexOf(absr, "data/sd", "/") == 0);
	// Roots can't be empty.
	CHECK(pathroot::indexOf(std::vector<std::string>{""}, "x", cwd) == -1);
	// Wii style: devices, no cwd. "sd:foo" is deliberately on "sd:".
	const std::vector<std::string> wii = {"sd:", "usb:"};
	auto w = [&](const std::string& p) { return pathroot::indexOf(wii, p, ""); };
	CHECK(w("sd:/apps/x") == 0);
	CHECK(w("sd:/") == 0);
	CHECK(w("sd:") == 0);
	CHECK(w("sd:foo") == 0);
	CHECK(w("usb:/x") == 1);
	CHECK(w("usb:/x/../../y") == 1);  // .. never climbs above the device root
	CHECK(w("usb2:/x") == -1);
	CHECK(w("sdx:/x") == -1);
	CHECK(w("/apps/x") == -1);
	CHECK(w("apps/x") == -1);
	CHECK(w("") == -1);
	// Longest root wins when roots nest.
	const std::vector<std::string> nest = {"./a", "./a/b"};
	CHECK(pathroot::indexOf(nest, "a/b/c", cwd) == 1);
	CHECK(pathroot::indexOf(nest, "a/c", cwd) == 0);
}

int main() {
	testProbeExclusive();
	testRootOf();
	testProbeOptions();
	testStackProbe();
	testDevOptions();
	testNetTestEnv();
	testInstallGuard();
	testLoadTracker();
	testRefreshRateLimit();
	if (failures) { std::printf("core_test: %d failure(s)\n", failures); return 1; }
	std::printf("core_test: all passed\n");
	return 0;
}
