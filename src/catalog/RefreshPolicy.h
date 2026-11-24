#pragma once
#include <cstdint>

// A Refresh within this many seconds of the last successful catalog fetch skips the network (the user decision
// of 2026-11-24; the dev option `ratelimit=<seconds>` overrides it for testing).
constexpr int kRefreshMinIntervalSec = 60;

// Pure so the host tests can cover it. `sinceLastFetchMs` is the time since the last successful fetch, or a
// negative number when there has been none; `limitSec` of 0 disables the limit.
inline bool refreshRateLimited(int64_t sinceLastFetchMs, int limitSec) {
	return limitSec > 0 && sinceLastFetchMs >= 0 && sinceLastFetchMs < (int64_t)limitSec * 1000;
}
