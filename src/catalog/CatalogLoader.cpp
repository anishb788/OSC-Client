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
	l.cacheTtlSec = o.cacheTtlSec;
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
	NetLoaderConfig cfg;
	cfg.baseUrl = opts.apiBase.empty() ? oscapi::kBaseUrl : opts.apiBase;
	cfg.cacheDir = opts.cacheDir;
	cfg.refreshMode = opts.mode == LoaderOptions::Mode::Refresh;
	cfg.offline = opts.offline;
	cfg.ignoreTtl = opts.ignoreTtl;
	if (opts.cacheTtlSec >= 0) cfg.ttlSec = opts.cacheTtlSec;
	// Startup uses a fresh cache and falls back to a stale one; Refresh ignores the TTL and has no stale fallback;
	// offline reads the cache only. That is catcache::decideSource, run by the job on the worker thread.
	LOG("Catalog: %s load from %s%s", cfg.refreshMode ? "refresh" : "startup", cfg.baseUrl.c_str(), opts.offline ? " (offline)" : "");
	return makeNetworkLoader(opts.worker, cfg);
}
