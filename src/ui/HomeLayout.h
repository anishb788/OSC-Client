#pragma once
// Pure layout of the Home screen (no SDL, so the host tests can include it). 640x480 (4:3) is the primary layout;
// 854x480 is the same design stretched, so both use the same column fractions and explicit heights.
#include <algorithm>
#include <cmath>

namespace homelayout {

inline constexpr float kTop = 74;          // top of both columns (below the 56 px top bar and its 18 px gap)
inline constexpr float kGap = 10;          // between the left column's items
inline constexpr float kFeaturedH = 150;
inline constexpr float kSearchH = 52;
inline constexpr float kRowH = 44;         // Popular / Newest and My apps
inline constexpr float kColGap = 18;       // between the left column and the tile grid
inline constexpr float kTileGap = 12;
inline constexpr float kAboveBottomBar = 16;
inline constexpr float kWideColumn = 300;  // a left column at least this wide (16:9) gets the 128x48 icon and 19 px name
inline constexpr float kWideTile = 190;    // tiles at least this wide (16:9) put the icon left of the name

struct Box {
	float x = 0, y = 0, w = 0, h = 0;
	float right() const { return x + w; }
	float bottom() const { return y + h; }
};

struct Layout {
	Box featured, search, popular, newest, myApps; // the left column
	Box grid;                                      // the 2x3 tile area
	float tileW = 0, tileH = 0;
	bool wideColumn = false;
	bool wideTiles = false;
	float columnBottom() const { return myApps.bottom(); }
};

inline Layout layout(float width, float height, float margin, float bottomBarH) {
	Layout l;
	float lw = std::round(width * 0.40f);
	float y = kTop;
	l.featured = {margin, y, lw, kFeaturedH};
	y += kFeaturedH + kGap;
	l.search = {margin, y, lw, kSearchH};
	y += kSearchH + kGap;
	float half = (lw - kGap) / 2;
	l.popular = {margin, y, half, kRowH};
	l.newest = {margin + half + kGap, y, half, kRowH};
	y += kRowH + kGap;
	l.myApps = {margin, y, lw, kRowH};
	l.wideColumn = lw >= kWideColumn;

	float gx = margin + lw + kColGap;
	float bottom = height - bottomBarH - kAboveBottomBar;
	l.grid = {gx, kTop, width - gx - margin, bottom - kTop};
	l.tileW = (l.grid.w - kTileGap) / 2;
	l.tileH = (l.grid.h - 2 * kTileGap) / 3;
	l.wideTiles = l.tileW >= kWideTile;
	return l;
}

} // namespace homelayout
