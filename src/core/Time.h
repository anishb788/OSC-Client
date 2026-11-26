#pragma once
// Wall-clock helpers shared by the loader, the cache, the parser and the icon cache (they all log how long a step took).
#include <chrono>

namespace timeutil {
inline double msSince(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
} // namespace timeutil
