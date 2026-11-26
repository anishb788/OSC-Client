// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Stack high-water measurement by painting. Pure (no SDL, no platform calls), so the host tests cover it.
//
// Usage: a thread paints a region of its own stack that is still unused (below the current stack pointer), runs its
// work, and later asks how deep the work got. Assumes the stack grows downwards (PowerPC EABI and x86-64 both do):
// `base` is the LOW end of the region, so the first bytes that stop being kPaint (searching up from `base`) mark the
// deepest write.
#include <cstddef>
#include <cstring>

namespace stackprobe {

constexpr unsigned char kPaint = 0xA5;

// Fills [base, base + size) with kPaint.
inline void paint(void* base, size_t size) { std::memset(base, kPaint, size); }

// Bytes of the region that have been written since paint(): the distance from the top of the region (base + size) down
// to the lowest byte that is no longer kPaint. 0 = untouched. A write that happens to store 0xA5 is not seen, so the
// result is a lower bound (never an over-estimate). `size` itself means the whole region was used (it may have
// overflowed beyond it).
inline size_t highWater(const void* base, size_t size) {
	const unsigned char* p = static_cast<const unsigned char*>(base);
	size_t untouched = 0;
	while (untouched < size && p[untouched] == kPaint) untouched++;
	return size - untouched;
}

} // namespace stackprobe
