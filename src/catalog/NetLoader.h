#pragma once
// The live catalog loader (Increment 9): one Catalog-class worker job that fetches /information, /contents and
// /featured-app, parses them and hands the finished Catalog to the main thread. Kept apart from CatalogLoader.cpp
// (which also pulls in the mock and DevOptions) so the host tests can link just this file with the worker.
#include <memory>
#include <string>

#include "catalog/CatalogLoader.h"

namespace net { class Worker; }

struct NetLoaderConfig {
	// Without a trailing slash: "<baseUrl>/information", "/contents" and "/featured-app" are requested.
	std::string baseUrl;
	// Per request, wall clock (a server that drips bytes defeats the idle timer).
	int totalTimeoutMs = 60000;
};

// Submits the job on construction. A null or not-running worker, or a submit that returns 0, gives a loader that
// reports Failed("Couldn't start the network worker.") on its first poll instead of waiting forever. The destructor
// cancels the job (it does not wait for it: the job only touches state it shares with the loader, so a late finish is
// harmless).
std::unique_ptr<CatalogLoader> makeNetworkLoader(net::Worker* worker, const NetLoaderConfig& cfg);

// Reports Failed(text) on the first poll.
std::unique_ptr<CatalogLoader> makeFailedLoader(const std::string& text);
