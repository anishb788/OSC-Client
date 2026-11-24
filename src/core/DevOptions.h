#pragma once
#include <string>

// Development switches, parsed once at startup.
//
// Sources, highest priority first:
//   1. Environment (PC build, set by `./dev`): OPENSHOP_START, OPENSHOP_MOCK, OPENSHOP_OFFLINE,
//      OPENSHOP_REFRESH, OPENSHOP_FAIL, OPENSHOP_TOPBAR, OPENSHOP_MOCKDELAY, OPENSHOP_RATELIMIT. An empty variable is ignored. On/off flags
//      (mock, offline, refresh) are on unless the value is "0", "false", "no" or "off"; fail, topbar, mockdelay
//      and ratelimit take the values described next to their fields.
//   2. <dataDir>/devstart (Wii, written by `./dev`; env vars don't reach Dolphin): one key=value
//      per line with keys screen, mock, offline, refresh, fail, topbar, mockdelay, ratelimit. A line without '=' is a bare screen name
//      (the old one-line format).
struct DevOptions {
	std::string screen; // "" = normal start
	bool mock = false;    // use the compiled-in mock catalog (acts from Increment 9)
	bool offline = false; // cache only, no network (acts from Increment 10)
	bool refresh = false; // ignore the cache TTL on startup too (acts from Increment 10; LoaderOptions::ignoreTtl)
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

	// topbar=all previews every top-bar action button with fake progress and badge; any other value (or none)
	// leaves the buttons to the features that own them.
	bool topbarAll = false;

	static DevOptions load(const std::string& dataDir);
	// Parses the devstart file text into `o`. Exposed separately so it can be tested without files.
	static void parseFile(const std::string& text, DevOptions& o);
	// Parses a `fail` value into failStartup / failRefresh.
	static void parseFail(const std::string& val, DevOptions& o);
	std::string describe() const;
};
