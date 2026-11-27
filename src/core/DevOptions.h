#pragma once
#include <string>

// Development switches, parsed once at startup.
//
// Sources, highest priority first:
//   1. Environment (PC build, set by `./dev`): OPENSHOP_START, OPENSHOP_MOCK, OPENSHOP_OFFLINE,
//      OPENSHOP_REFRESH, OPENSHOP_FAIL, OPENSHOP_TOPBAR, OPENSHOP_MOCKDELAY, OPENSHOP_RATELIMIT, OPENSHOP_CACHETTL, OPENSHOP_API, OPENSHOP_ICONS, OPENSHOP_AUTOREFRESH, OPENSHOP_PERF, OPENSHOP_HEAPPROBE, OPENSHOP_STACKPROBE, OPENSHOP_NETTEST, OPENSHOP_NETTESTBG, OPENSHOP_NETTESTQUIT, OPENSHOP_APPS_DEVICE, OPENSHOP_APPS_DEVICE_SWAP. An empty variable is ignored. On/off flags
//      (mock, offline, refresh) are on unless the value is "0", "false", "no" or "off"; fail, topbar, mockdelay
//      and ratelimit take the values described next to their fields.
//   2. <dataDir>/devstart (Wii, written by `./dev`; env vars don't reach Dolphin): one key=value
//      per line with keys screen, mock, offline, refresh, fail, topbar, mockdelay, ratelimit, cachettl, api, icons, autorefresh, perf, heapprobe, stackprobe, nettest, nettestbg, nettestquit, appsdevice, appsdeviceswap. A line without '=' is a bare screen name
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

	// One probe per launch: the two disturb each other's numbers (and the heap probe fills the whole heap), so asking for
	// both runs NEITHER and App logs why.
	enum class ProbeChoice { None, Heap, Stack, Conflict };
	ProbeChoice probeChoice() const {
		return heapProbe && stackProbe ? ProbeChoice::Conflict : heapProbe ? ProbeChoice::Heap : stackProbe ? ProbeChoice::Stack : ProbeChoice::None;
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
