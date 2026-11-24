#pragma once
#include <cstdint>

#include "catalog/InstallGuard.h"

// The bookkeeping behind App's catalog loads, kept free of SDL and of App so the host tests can drive it with fake
// screen pointers. It owns:
//   - which LoadingScreen is "recorded" (the only screen allowed to install), including the refresh hand-over
//     (a refresh's screen becomes the recorded one only when its reset swap actually happens),
//   - the load token (so a loader that a Retry replaced can't install late),
//   - the catalog generation (bumped on every accepted install),
//   - when the catalog was last fetched from the network (for the Refresh rate limit; a catalog served from the SD
//     cache is installed but does not count as a fetch).
// Screens are identified by opaque pointers that are only ever compared, never dereferenced.
class LoadTracker {
public:
	// Startup: App created this LoadingScreen and pushed it as the only screen.
	void recordStartupScreen(const void* screen) { recorded_ = screen; pending_ = nullptr; }
	// Refresh: this screen will become the recorded one if (and when) the reset to it happens.
	void setPendingScreen(const void* screen) { pending_ = screen; }
	// A reset swap just made `bottom` the only screen. Every other screen is gone, so the old recorded screen is too.
	void onStackReset(const void* bottom) {
		recorded_ = (pending_ && pending_ == bottom) ? pending_ : nullptr;
		pending_ = nullptr;
	}

	// The recorded LoadingScreen starts (or restarts, on Retry) a load. Returns its token.
	uint32_t beginLoad(const void* from) {
		owner_ = from;
		return ++token_;
	}

	// Why an install would be refused right now, or nullptr. `onlyScreen` is the stack's single screen, or nullptr
	// when the stack holds anything other than exactly one screen.
	const char* refusal(const void* onlyScreen, bool screenChangePending, uint32_t token) const {
		InstallState st;
		st.onlyLoadingScreen = recorded_ && onlyScreen == recorded_;
		st.screenChangePending = screenChangePending;
		st.loaderMatches = recorded_ && owner_ == recorded_ && token != 0 && token == token_;
		return catalogInstallRefusal(st);
	}

	// An install was accepted at `nowMs` (a free-running millisecond clock that may wrap).
	void accepted(uint32_t nowMs, bool fetchedFromNetwork) {
		generation_++;
		if (fetchedFromNetwork) {
			haveFetch_ = true;
			lastFetchMs_ = nowMs;
		}
	}

	bool installed() const { return generation_ > 0; }
	uint32_t generation() const { return generation_; }
	const void* recordedScreen() const { return recorded_; }
	// Milliseconds since the last network fetch (wrap-safe), or -1 if there has been none.
	int64_t msSinceFetch(uint32_t nowMs) const { return haveFetch_ ? (int64_t)(uint32_t)(nowMs - lastFetchMs_) : -1; }

private:
	const void* recorded_ = nullptr;
	const void* pending_ = nullptr;
	const void* owner_ = nullptr;
	uint32_t token_ = 0;
	uint32_t generation_ = 0;
	bool haveFetch_ = false;
	uint32_t lastFetchMs_ = 0;
};
