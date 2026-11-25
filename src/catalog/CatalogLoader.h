#pragma once
#include <memory>
#include <string>

#include "catalog/Catalog.h"

struct DevOptions;
namespace net { class Worker; }

// What the LoadingScreen waits on. Contract:
// - poll() is called once per frame on the main thread and must return quickly. A loader that does slow work
//   runs it elsewhere (the network worker, from Increment 9) and only reports its state here.
// - After poll() has returned Done, take() is called exactly once to move the catalog out. After Unchanged
//   ("finished, keep the catalog that is installed"; the rate-limited Refresh) take() is never called.
// - The LoadingScreen destroys a loader at any time (Retry replaces it, B cancels a Refresh, Exit quits). The
//   destructor must cancel any in-flight work, and nothing may write into a dead loader afterwards. The network
//   loader does not wait for its worker job: the job only touches state it shares with the loader (shared_ptr).
struct LoadStatus {
	enum class State { Pending, Done, Unchanged, Failed };
	State state = State::Pending;
	std::string text; // Pending: status line (empty = keep the screen's default). Failed: reason. Otherwise unused.
	// Done: true when the catalog came from the network (the mock stands in for it), false when it came from the
	// SD cache (Increment 10). Only a network fetch restarts the Refresh rate limit.
	bool fromNetwork = true;
};

class CatalogLoader {
public:
	virtual ~CatalogLoader() = default;
	virtual LoadStatus poll() = 0;
	virtual Catalog take() = 0;
};

// The subset of DevOptions that decides how the catalog is loaded.
struct LoaderOptions {
	// Startup: the first load, no catalog exists yet. Refresh: a user-triggered reload; the previous catalog stays
	// installed until (and unless) this one succeeds.
	enum class Mode { Startup, Refresh };

	bool mock = false;    // compiled-in mock data instead of the network
	bool offline = false; // cache only, never the network; an error if there is no usable cache
	// Dev option `refresh=1`: ignore the cache TTL on startup too. Not the same as Mode::Refresh, which always ignores
	// the TTL and never falls back to a stale cache.
	bool ignoreTtl = false;
	int cacheTtlSec = -1;     // dev option `cachettl=`: replaces the 1 h catalog TTL (-1 = the built-in one)
	std::string cacheDir;     // <dataDir>/cache (set by App, not by from()); empty = no cache
	bool failStartup = false; // dev hook: the first startup attempt fails, retries succeed
	bool failRefresh = false; // dev hook: every refresh fails
	int mockDelayMs = -1;     // dev hook: artificial mock delay (-1 = default)
	std::string apiBase;      // dev hook `api=`: replaces oscapi::kBaseUrl ("" = the real server)
	net::Worker* worker = nullptr; // the network worker the live loader submits to (set by App, not by from())
	Mode mode = Mode::Startup;
	// Refresh only: the last network fetch was too recent (see RefreshPolicy.h), so nothing is fetched. The loader
	// just waits briefly and reports Unchanged; the LoadingScreen keeps the installed catalog.
	bool reuseCurrent = false;

	bool failing() const { return mode == Mode::Refresh ? failRefresh : failStartup; }
	static LoaderOptions from(const DevOptions& o);
};

// The one place that chooses a loader. `attempt` is 0 for the first load and counts up on each Retry.
std::unique_ptr<CatalogLoader> makeCatalogLoader(const LoaderOptions& opts, int attempt);

// Mock data after a short artificial delay (so the loading screen is visible). With `fail` it fails instead.
std::unique_ptr<CatalogLoader> makeMockLoader(bool fail, int delayMs = 300);
// Waits `delayMs`, then reports Unchanged (take() is never called). For the rate-limited Refresh.
std::unique_ptr<CatalogLoader> makeIdleLoader(int delayMs);
