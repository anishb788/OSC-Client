#pragma once
#include <memory>
#include <string>

#include "catalog/Catalog.h"

struct DevOptions;

// What the LoadingScreen waits on. Contract:
// - poll() is called once per frame on the main thread and must return quickly. A loader that does slow work
//   runs it elsewhere (the network worker, from Increment 9) and only reports its state here.
// - After poll() has returned Done, take() is called exactly once to move the catalog out.
// - The LoadingScreen destroys a loader at any time (Retry replaces it, Exit quits). The destructor must cancel
//   and join any in-flight work before returning, so nothing writes into a dead loader.
struct LoadStatus {
	enum class State { Pending, Done, Failed };
	State state = State::Pending;
	std::string text; // Pending: status line. Failed: human-readable reason. Done: unused.
};

class CatalogLoader {
public:
	virtual ~CatalogLoader() = default;
	virtual LoadStatus poll() = 0;
	virtual Catalog take() = 0;
};

// The subset of DevOptions that decides how the catalog is loaded.
struct LoaderOptions {
	bool mock = false;    // compiled-in mock data instead of the network (the only source until Increment 9)
	bool offline = false; // cache only (Increment 10)
	bool refresh = false; // ignore the cache TTL (Increment 10)
	bool fail = false;    // dev hook: the first attempt fails, retries succeed

	static LoaderOptions from(const DevOptions& o);
};

// The one place that chooses a loader. `attempt` is 0 for the first load and counts up on each Retry.
std::unique_ptr<CatalogLoader> makeCatalogLoader(const LoaderOptions& opts, int attempt);

// Mock data after a short artificial delay (so the loading screen is visible). With `fail` it fails instead.
std::unique_ptr<CatalogLoader> makeMockLoader(bool fail, int delayMs = 300);
