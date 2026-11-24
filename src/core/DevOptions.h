#pragma once
#include <string>

// Development switches, parsed once at startup.
//
// Sources, highest priority first:
//   1. Environment (PC build, set by `./dev`): OPENSHOP_START, OPENSHOP_MOCK, OPENSHOP_OFFLINE,
//      OPENSHOP_REFRESH, OPENSHOP_FAIL. A flag is on when the variable is set to anything but "" or "0".
//   2. <dataDir>/devstart (Wii, written by `./dev`; env vars don't reach Dolphin): one key=value
//      per line with keys screen, mock, offline, refresh, fail. A line without '=' is a bare screen name
//      (the old one-line format).
struct DevOptions {
	std::string screen; // "" = normal start
	bool mock = false;    // use the compiled-in mock catalog (acts from Increment 9)
	bool offline = false; // cache only, no network (acts from Increment 10)
	bool refresh = false; // ignore the cache TTL (acts from Increment 10)
	bool fail = false;    // the first catalog load fails, to exercise the error screen (Retry then succeeds)

	static DevOptions load(const std::string& dataDir);
	// Parses the devstart file text into `o`. Exposed separately so it can be tested without files.
	static void parseFile(const std::string& text, DevOptions& o);
	std::string describe() const;
};
