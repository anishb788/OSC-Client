#include "catalog/CatalogLoader.h"

#include "catalog/NetLoader.h"
#include "catalog/OscApi.h"
#include "core/DevOptions.h"
#include "core/Log.h"

LoaderOptions LoaderOptions::from(const DevOptions& o) {
	LoaderOptions l;
	l.mock = o.mock;
	l.offline = o.offline;
	l.ignoreTtl = o.refresh;
	l.failStartup = o.failStartup;
	l.failRefresh = o.failRefresh;
	l.mockDelayMs = o.mockDelayMs;
	l.apiBase = o.apiBase;
	return l;
}

std::unique_ptr<CatalogLoader> makeCatalogLoader(const LoaderOptions& opts, int attempt) {
	// The idle pass is short but visible, so the rate-limited Refresh takes the same route as a real one.
	constexpr int kIdleMs = 400, kMockMs = 300;
	if (opts.reuseCurrent) return makeIdleLoader(opts.mockDelayMs >= 0 ? opts.mockDelayMs : kIdleMs);
	const int delay = opts.mockDelayMs >= 0 ? opts.mockDelayMs : kMockMs;
	const bool fail = opts.failing() && (opts.mode == LoaderOptions::Mode::Refresh || attempt == 0);
	// The fail hook never reaches the network, and a failure never falls back to mock data: only `mock` asks for it.
	if (opts.mock || fail) return makeMockLoader(fail, delay);
	// Increment 10 puts the cache in front of this: Startup uses a fresh cache, Refresh ignores the TTL and has no
	// stale-cache fallback. Until then both modes fetch, and offline (cache only) has nothing to read.
	if (opts.offline) return makeFailedLoader("Offline mode is on (the offline dev option) and there is no cached catalog yet.");
	NetLoaderConfig cfg;
	cfg.baseUrl = opts.apiBase.empty() ? oscapi::kBaseUrl : opts.apiBase;
	LOG("Catalog: %s load from %s", opts.mode == LoaderOptions::Mode::Refresh ? "refresh" : "startup", cfg.baseUrl.c_str());
	return makeNetworkLoader(opts.worker, cfg);
}
