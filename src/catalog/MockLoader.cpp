#include <SDL.h>

#include "catalog/CatalogLoader.h"
#include "core/Log.h"

namespace {
class MockLoader : public CatalogLoader {
public:
	MockLoader(bool fail, int delayMs) : fail_(fail), delayMs_(delayMs), start_(SDL_GetTicks()) {}

	LoadStatus poll() override {
		if ((int)(SDL_GetTicks() - start_) < delayMs_) return {LoadStatus::State::Pending, "Loading catalog…"};
		if (fail_) return {LoadStatus::State::Failed, "Simulated failure (the fail dev option is set)."};
		return {LoadStatus::State::Done, {}};
	}

	Catalog take() override {
		Catalog c;
		loadMockCatalog(c);
		LOG("Catalog: %d apps (mock data)", (int)c.apps.size());
		return c;
	}

private:
	bool fail_;
	int delayMs_;
	Uint32 start_;
};
} // namespace

std::unique_ptr<CatalogLoader> makeMockLoader(bool fail, int delayMs) { return std::make_unique<MockLoader>(fail, delayMs); }
