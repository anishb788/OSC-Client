#pragma once

// Pure decision behind App::installCatalog (kept free of SDL so the host tests can cover it).
// Returns nullptr when installing is allowed, otherwise the reason it is refused.
struct InstallState {
	bool alreadyInstalled = false;
	bool onlyLoadingScreen = true; // the stack holds exactly the startup LoadingScreen
	bool screenChangePending = false;
};

inline const char* catalogInstallRefusal(const InstallState& s) {
	if (s.alreadyInstalled) return "the catalog was already installed (it is installed once per session)";
	if (!s.onlyLoadingScreen) return "a screen other than the LoadingScreen exists and may point into the catalog";
	if (s.screenChangePending) return "a screen change is pending";
	return nullptr;
}
