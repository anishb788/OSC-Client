#pragma once
// The eviction decision of the text texture cache, separate from SDL so a host test can drive it without a font.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace textevict {

struct Candidate {
	uint32_t lastUsed;   // the frame counter value when the string was last drawn or looked up
	size_t bytes;
};

// `frame` is the counter AFTER this frame's increment, so a string drawn this frame has lastUsed == frame - 1. Those are
// never evicted (they are on screen right now), so the cache can stay over budget when everything in it is in use.
inline bool evictable(uint32_t lastUsed, uint32_t frame) { return lastUsed + 1 < frame; }

// Which entries to drop, as indices into `all`, least recently used first, until what is left is at most `target` bytes
// (`total` is the sum of all bytes). Does nothing, and in particular sorts nothing, when `total` is within `budget` or
// when no entry is older than the last frame: the caller runs this every frame, and an over-budget cache full of
// on-screen text must not cost a sort per frame.
inline std::vector<size_t> choose(const std::vector<Candidate>& all, size_t total, size_t budget, size_t target, uint32_t frame) {
	std::vector<size_t> victims;
	if (total <= budget) return victims;
	for (size_t i = 0; i < all.size(); i++)
		if (evictable(all[i].lastUsed, frame)) victims.push_back(i);
	if (victims.empty()) return victims;
	std::sort(victims.begin(), victims.end(), [&](size_t a, size_t b) {
		return all[a].lastUsed != all[b].lastUsed ? all[a].lastUsed < all[b].lastUsed : a < b;
	});
	size_t left = total, n = 0;
	while (n < victims.size() && left > target) left -= std::min(left, all[victims[n++]].bytes);
	victims.resize(n);
	return victims;
}

}  // namespace textevict
