#include <SDL.h>

#include <ctime>

#include "catalog/CatalogLoader.h"
#include "core/Log.h"

namespace {
class MockLoader : public CatalogLoader {
public:
	MockLoader(bool fail, int delayMs, bool idle = false)
	    : fail_(fail), idle_(idle), delayMs_(delayMs), start_(SDL_GetTicks()) {}

	LoadStatus poll() override {
		if ((int)(SDL_GetTicks() - start_) < delayMs_) return {LoadStatus::State::Pending, {}}; // keep the screen's default status line
		if (fail_) return {LoadStatus::State::Failed, "Simulated failure (the fail dev option is set)."};
		if (idle_) return {LoadStatus::State::Unchanged, {}};
		return {LoadStatus::State::Done, {}, /*fromNetwork=*/true}; // the mock stands in for a network fetch
	}

	Catalog take() override {
		Catalog c; // never called on an idle loader (it reports Unchanged)
		loadMockCatalog(c);
		c.fetchedAt = (int64_t)std::time(nullptr);
		LOG("Catalog: %d apps (mock data)", (int)c.apps.size());
		return c;
	}

private:
	bool fail_, idle_;
	int delayMs_;
	Uint32 start_;
};
} // namespace

std::unique_ptr<CatalogLoader> makeMockLoader(bool fail, int delayMs) { return std::make_unique<MockLoader>(fail, delayMs); }
std::unique_ptr<CatalogLoader> makeIdleLoader(int delayMs) {
	return std::make_unique<MockLoader>(false, delayMs, true);
}
