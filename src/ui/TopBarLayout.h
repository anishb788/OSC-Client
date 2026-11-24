#pragma once
// Pure layout for the top bar's right-aligned action capsules. No SDL, so the host tests can include it.
//
// The capsules are listed in on-screen order (Downloads, Refresh, Settings). Hidden ones collapse to the right:
// the last visible capsule always ends at `width - margin`, so the bar never has a gap.
#include <algorithm>
#include <array>

namespace topbar {

inline constexpr int kCount = 3;            // Downloads, Refresh, Settings
inline constexpr float kCapW = 52, kCapH = 38; // 4:3 (640x480) is the primary layout; 16:9 stretches it
inline constexpr float kGap = 8;            // between capsules
inline constexpr float kTop = 8;            // capsule y
inline constexpr float kTitleGap = 14;      // title / subtitle stop at least this far left of the first capsule
inline constexpr float kTitleFrac = 0.62f;  // the title takes at most this share of the width while a subtitle shows
inline constexpr float kMinSubtitle = 100;  // narrower than this (or than the subtitle itself), the subtitle is dropped

struct Box {
	float x = 0, y = 0, w = 0, h = 0;
};

struct Layout {
	std::array<Box, kCount> rect{};      // valid where visible[i]
	std::array<bool, kCount> visible{};
	int count = 0;
	float titleRight = 0;                // title and subtitle must end at or before this x
};

inline Layout layout(float width, float margin, const std::array<bool, kCount>& visible) {
	Layout l;
	l.visible = visible;
	for (bool v : visible) l.count += v ? 1 : 0;
	float right = width - margin;
	float x = right - l.count * kCapW - (l.count > 0 ? (l.count - 1) * kGap : 0);
	l.titleRight = l.count > 0 ? x - kTitleGap : right;
	for (int i = 0; i < kCount; i++) {
		if (!visible[i]) continue;
		l.rect[i] = {x, kTop, kCapW, kCapH};
		x += kCapW + kGap;
	}
	return l;
}

struct Fit {
	float titleMaxW = 0;      // ellipsize the title to this width
	bool showSubtitle = false;
};

// Decides how title and subtitle share [margin, titleRight]. The subtitle is dropped before the title is
// ellipsized further: with a subtitle the title is capped at kTitleFrac of the width (as before the capsules
// existed); without one it may use everything up to titleRight.
inline Fit fit(float width, float margin, float titleRight, float titleW, float subtitleW) {
	float avail = std::max(0.0f, titleRight - margin);
	Fit f;
	float capped = std::min({titleW, width * kTitleFrac, avail});
	float room = avail - capped - kTitleGap;
	f.showSubtitle = subtitleW > 0 && room >= std::min(subtitleW, kMinSubtitle);
	f.titleMaxW = f.showSubtitle ? std::min(width * kTitleFrac, avail) : avail;
	return f;
}

} // namespace topbar
