#include "catalog/CatalogLoader.h"

#include "core/DevOptions.h"

LoaderOptions LoaderOptions::from(const DevOptions& o) {
	LoaderOptions l;
	l.mock = o.mock;
	l.offline = o.offline;
	l.ignoreTtl = o.refresh;
	l.failStartup = o.failStartup;
	l.failRefresh = o.failRefresh;
	l.mockDelayMs = o.mockDelayMs;
	return l;
}

std::unique_ptr<CatalogLoader> makeCatalogLoader(const LoaderOptions& opts, int attempt) {
	// The idle pass is short but visible, so the rate-limited Refresh takes the same route as a real one.
	constexpr int kIdleMs = 400, kMockMs = 300;
	if (opts.reuseCurrent) return makeIdleLoader(opts.mockDelayMs >= 0 ? opts.mockDelayMs : kIdleMs);
	const int delay = opts.mockDelayMs >= 0 ? opts.mockDelayMs : kMockMs;
	// Increment 9 adds the network loader here, used unless opts.mock is set; Refresh mode then means "fetch, ignore
	// the TTL, no stale-cache fallback" (Increment 10). Until then both modes load the mock catalog.
	return makeMockLoader(opts.failing() && (opts.mode == LoaderOptions::Mode::Refresh || attempt == 0), delay);
}
