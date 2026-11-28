#pragma once
#include <string>

// Development switches, parsed once at startup.
//
// Sources, highest priority first:
//   1. Environment (PC build, set by `./dev`): OPENSHOP_START, OPENSHOP_MOCK, OPENSHOP_OFFLINE,
//      OPENSHOP_REFRESH, OPENSHOP_FAIL, OPENSHOP_TOPBAR, OPENSHOP_MOCKDELAY, OPENSHOP_RATELIMIT, OPENSHOP_CACHETTL, OPENSHOP_API, OPENSHOP_ICONS, OPENSHOP_AUTOREFRESH, OPENSHOP_PERF, OPENSHOP_HEAPPROBE, OPENSHOP_STACKPROBE, OPENSHOP_COMMITPROBE, OPENSHOP_COMMITPROBE_FILES, OPENSHOP_COMMITPROBE_STACK, OPENSHOP_CLAIMPROBE, OPENSHOP_NETTEST, OPENSHOP_NETTESTBG, OPENSHOP_NETTESTQUIT, OPENSHOP_APPS_DEVICE, OPENSHOP_APPS_DEVICE_SWAP, OPENSHOP_DEVJOBS, OPENSHOP_DEVJOBMS, OPENSHOP_DEVJOBFAIL, OPENSHOP_DEVJOBCOMMIT, OPENSHOP_DEV_INSTALL, OPENSHOP_TEST_BAD_MD5. An empty variable is ignored. On/off flags
//      (mock, offline, refresh) are on unless the value is "0", "false", "no" or "off"; fail, topbar, mockdelay
//      and ratelimit take the values described next to their fields.
//   2. <dataDir>/devstart (Wii, written by `./dev`; env vars don't reach Dolphin): one key=value
//      per line with keys screen, mock, offline, refresh, fail, topbar, mockdelay, ratelimit, cachettl, api, icons, autorefresh, perf, heapprobe, stackprobe, commitprobe, commitprobe_files, commitprobe_stack, claimprobe, nettest, nettestbg, nettestquit, appsdevice, appsdeviceswap, devjobs, devjobms, devjobfail, devjobcommit, install, badmd5. A line without '=' is a bare screen name
//      (the old one-line format).
struct DevOptions {
	std::string screen; // "" = normal start
	bool mock = false;    // use the compiled-in mock catalog (acts from Increment 9)
	bool offline = false; // cache only, no network; an error if there is no usable cache (Increment 10)
	bool refresh = false; // ignore the cache TTL on startup too (LoaderOptions::ignoreTtl; a stale fallback still applies)
	// fail=startup|refresh|both (also "1" and any other truthy value, meaning startup; "0", "" and "none" mean
	// neither): the first startup load fails (error screen; Retry then succeeds) and/or every Refresh fails.
	bool failStartup = false;
	bool failRefresh = false;
	// mockdelay=<ms>: how long the mock loader (and the rate-limited Refresh) takes. -1 = the built-in default.
	// Lets HOME and B be tested while a load is running.
	int mockDelayMs = -1;
	// ratelimit=<seconds>: overrides the 60 s Refresh rate limit (0 = never limited). -1 = the built-in default (60,
	// or 0 when fail=refresh is set, so that hook can fire straight after startup).
	int rateLimitSec = -1;

	// cachettl=<seconds>: replaces the 1 h catalog cache TTL (0 = the cache is always stale). -1 = the built-in TTL.
	int cacheTtlSec = -1;

	// api=<base url>: replaces the OSC API base (http://hbb1.oscwii.org/api/v4) for the catalog fetch, e.g. a dead port
	// to see the error screen or tools/http_test_server.py to see "bad data". Empty = the real server.
	std::string apiBase;

	// icons=<base url>: icons are fetched from "<base>/<slug>/icon.png" instead of the catalog's URLs (a local test server,
	// or a dead port to see the placeholders). Empty = the catalog's URLs. Ignored by mock mode, which has no real icons.
	std::string iconBase;

	// autorefresh=<ms>: whenever Home has been the only screen for this long, trigger a Refresh by itself (like the
	// top-bar button). Lets the Refresh path be checked where nothing can click (Dolphin, CI). -1 = off.
	int autoRefreshMs = -1;

	// topbar=all previews every top-bar action button with fake progress and badge; any other value (or none)
	// leaves the buttons to the features that own them.
	bool topbarAll = false;

	// perf=1: log the average CPU draw time, present (vsync) time and frame time every 120 frames, plus the number of
	// textures Gfx has created so far (it must stay flat once a screen has been drawn).
	bool perf = false;

	// heapprobe=1: once at startup, allocate 1 MB blocks until malloc fails (capped at 80 MB), log the MEM1 / MEM2 split, the
	// MEM1->MEM2 gap and sbrk before / peak / after, then free everything (platform::heapProbe). Does nothing unless set.
	bool heapProbe = false;

	// stackprobe=1: once at startup, run a short-lived 64 KB-stack SDL thread that logs the stack bounds (assumed on the Wii), its
	// painted-stack high-water and what SDL_SetThreadPriority does (platform::stackProbe). Does nothing unless set.
	bool stackProbe = false;

	// commitprobe=cap|flush (DEBUGTOOLS builds only, platform::commitProbe; a release logs that it is not built in): once at
	// startup, measure the commit engine on the device on a worker thread with the install worker's stack size.
	//   cap    builds a synthetic old install and a staged update near the plan caps under apps/osprobe, runs a full update commit
	//          and logs heap in use / elapsed ms per phase / stack high-water; everything is removed again.
	//   flush  writes three files (journal-style fsynced, plain fwrite+fclose, fsynced rename) and sleeps 90 s so the host can
	//          snapshot the emulated SD card; then removes them.
	// Stored as typed (trimmed); App rejects any other value with a log line. Empty = off.
	std::string commitProbe;
	// commitprobe_files=<n>: size of the cap probe (see src/platform/CommitProbe.h): n restore ops, ~2n manifest paths. Default 5000;
	// 19000 sits just under both caps (20000 ops, 40000 paths). -1 = default.
	int commitProbeFiles = -1;
	// commitprobe_stack=<KB>: the worker's stack size, default 64 (the plan's proposal for the install worker). -1 = default.
	int commitProbeStackKb = -1;

	// claimprobe=1 (DEBUGTOOLS builds only, platform::claimProbe; a release logs that it is not built in; M3 Increment 7): queues three
	// real jobs on the install worker (the main thread never waits) that show the staging claim at work on the SD card: a job holds a
	// claim on one staging dir while the Cleanup routine runs, the claimed dir must survive and an abandoned one must go; a normal
	// Cleanup job then removes the released dir; the probe removes anything left. Refuses when a journal exists on the SD card or
	// staging holds dirs it did not create. Logs "claimprobe:" lines, ending in RESULT PASS or FAIL and "cleaned".
	bool claimProbe = false;

	// One probe per launch: they disturb each other's numbers (and the heap probe fills the whole heap), so asking for more
	// than one runs NONE and App logs why.
	enum class ProbeChoice { None, Heap, Stack, Commit, Claim, Conflict };
	ProbeChoice probeChoice() const {
		const int n = (heapProbe ? 1 : 0) + (stackProbe ? 1 : 0) + (commitProbe.empty() ? 0 : 1) + (claimProbe ? 1 : 0);
		if (n > 1) return ProbeChoice::Conflict;
		return heapProbe    ? ProbeChoice::Heap
		       : stackProbe ? ProbeChoice::Stack
		       : !commitProbe.empty() ? ProbeChoice::Commit
		       : claimProbe ? ProbeChoice::Claim
		                    : ProbeChoice::None;
	}

	// nettest=<script>, nettestbg=<url>, nettestquit=1: the Increment 7 network self-test (net/NetSelfTest.h). Off (empty)
	// by default; it never changes the UI.
	std::string nettest;
	std::string nettestBg;
	bool nettestQuit = false;

	// appsdevice=sd|usb: forces the "Show apps on" device for this session if it is mounted; never saved. Stored as typed
	// (trimmed, case kept); App (install::parseDeviceId, case-insensitive) rejects anything but sd / usb with a log line. Empty = off.
	std::string appsDevice;
	// appsdeviceswap=1: once the first scan is done (Increment 11), switch to the other device once, to test switching
	// before M4's Settings row exists. Parsed now; does nothing until Increment 11 (App logs that it was requested).
	bool appsDeviceSwap = false;

	// devjobs=<n>: once the job system is up, queue n dev Sleep jobs (Increment 7; n is capped at kMaxDevJobs = 100), each devjobms long (default 3000). They run strictly
	// one after the other on the install worker; their progress is logged from the main thread's snapshot. devjobfail=1 makes the LAST
	// one fail; devjobcommit=1 makes the second half of each count as the commit phase (it cannot be cancelled and quitting waits for it).
	// They make "Show apps on" switching refuse (the busy guard) while they run. 0 = off.
	static constexpr int kMaxDevJobs = 100;
	int devJobs = 0;
	int devJobMs = -1; // -1 = 3000
	bool devJobFail = false;
	bool devJobCommit = false;

	// install=<slug> (env OPENSHOP_DEV_INSTALL; M3 Increment 8): once the first catalog is installed, queue an Install job for that app
	// exactly as the UI will (a JobSpec copied from the catalog entry, target = the apps device's root; use appsdevice=usb to target
	// USB). Fires once per launch, not again after a Refresh. Stored as typed (trimmed); App refuses a value that is not a valid slug
	// or not in the catalog, with a log line. Empty = off. Until Increment 8a an install ends after the download is verified.
	std::string installSlug;
	// badmd5=1 (env OPENSHOP_TEST_BAD_MD5): the install= job expects a hash the archive cannot match, so it must fail with the
	// "damaged" text and leave nothing behind.
	bool testBadMd5 = false;

	static DevOptions load(const std::string& dataDir);
	// Parses the devstart file text into `o`. Exposed separately so it can be tested without files.
	static void parseFile(const std::string& text, DevOptions& o);
	// Parses a `fail` value into failStartup / failRefresh.
	static void parseFail(const std::string& val, DevOptions& o);
	std::string describe() const;
	// Only the options that differ from a normal launch, e.g. "mock=1 fail=startup"; "" when none is set. Used for the
	// one warning line a normal build logs when a devstart file or the environment changed its behaviour.
	std::string active() const;
};
