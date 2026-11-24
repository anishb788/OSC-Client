#pragma once

// Pure decision behind App::installCatalog (kept free of SDL so the host tests can cover it).
// Returns nullptr when installing is allowed, otherwise the reason it is refused.
//
// The catalog may be (re-)installed, any number of times per session, only while nothing can still point into the
// old one. Every holder of `const AppEntry&` / `const AppEntry*` (Detail, List, Search, Home's featured card) is a
// Screen, and the reset to the LoadingScreen destroys every screen first. So an install is safe exactly when:
//   1. the stack holds nothing but the LoadingScreen App recorded for the current load,
//   2. no push, pop or reset is pending (a pending reset could put a screen on top, a pending push likewise), and
//   3. the request comes from that screen's current load, so a loader that a Retry replaced can't install late.
// Background work never holds catalog pointers either (it copies what it needs and refers to apps by slug), so
// nothing outside the screen stack has to be considered.
struct InstallState {
	bool onlyLoadingScreen = true; // the stack holds exactly the recorded LoadingScreen
	bool screenChangePending = false;
	bool loaderMatches = true; // the request carries the recorded LoadingScreen's current load token
};

inline const char* catalogInstallRefusal(const InstallState& s) {
	if (!s.onlyLoadingScreen) return "a screen other than the recorded LoadingScreen exists and may point into the catalog";
	if (s.screenChangePending) return "a screen change is pending";
	if (!s.loaderMatches) return "the request is not from the LoadingScreen's current load (a stale loader)";
	return nullptr;
}
