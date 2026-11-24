#include "catalog/CatalogLoader.h"

#include "core/DevOptions.h"

LoaderOptions LoaderOptions::from(const DevOptions& o) {
	return LoaderOptions{o.mock, o.offline, o.refresh, o.fail};
}

std::unique_ptr<CatalogLoader> makeCatalogLoader(const LoaderOptions& opts, int attempt) {
	// Increment 9 adds the network loader here, used unless opts.mock is set.
	return makeMockLoader(opts.fail && attempt == 0);
}
