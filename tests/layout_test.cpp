// Host tests for the pure Home layout (no SDL): the 16:9 end point, no overlap at 640x480.
#include <cmath>
#include <cstdio>

#include "ui/HomeLayout.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

using namespace homelayout;

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
static const float kMargin = 24, kBottomBar = 64;

static bool overlaps(const Box& a, const Box& b) {
	return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

static void testWide() {
	Layout l = layout(854, 480, kMargin, kBottomBar);
	CHECK(near(l.featured.x, 24) && near(l.featured.y, 74) && near(l.featured.w, 342) && near(l.featured.h, 150));
	CHECK(near(l.columnBottom(), 394)); // epic: My apps ends at y = 394, 22 px above the bottom bar
	CHECK(near(l.search.y, 74 + 150 + 10) && near(l.search.h, 52));
	CHECK(near(l.popular.h, 44) && near(l.newest.h, 44));
	CHECK(near(l.newest.x - l.popular.right(), 10)); // 10 px apart
	CHECK(near(l.newest.right(), l.search.right()));
	CHECK(near(l.myApps.y, l.popular.bottom() + 10) && near(l.myApps.w, 342) && near(l.myApps.h, 44));
	CHECK(l.wideColumn);
	CHECK(l.wideTiles);
	// Grid: x = 384, top 74, 16 px above the bottom bar, 2x3, 12 px gaps.
	CHECK(near(l.grid.x, 384) && near(l.grid.y, 74) && near(l.grid.bottom(), 480 - 64 - 16));
	CHECK(near(l.grid.right(), 854 - 24));
	CHECK(near(l.tileW * 2 + 12, l.grid.w) && near(l.tileH * 3 + 24, l.grid.h));
}

static void testNarrow() {
	Layout l = layout(640, 480, kMargin, kBottomBar);
	CHECK(near(l.featured.w, 256)); // round(640 * 0.40), the ~258 px column of the 4:3 mockup
	CHECK(!l.wideColumn);
	CHECK(!l.wideTiles); // decided on the layout rect, never on the hover-inflated one
	CHECK(near(l.columnBottom(), 394)); // heights are explicit, so 4:3 ends at the same y
	Box items[] = {l.featured, l.search, l.popular, l.newest, l.myApps};
	for (int i = 0; i < 5; i++) {
		CHECK(items[i].x >= kMargin - 0.001f && items[i].right() <= l.grid.x - 18 + 0.001f);
		for (int j = i + 1; j < 5; j++) CHECK(!overlaps(items[i], items[j]));
	}
	CHECK(l.columnBottom() <= 480 - kBottomBar); // clear of the bottom bar
	CHECK(near(l.grid.bottom(), 400));
	CHECK(l.grid.x >= l.featured.right() + 18 - 0.001f && near(l.grid.right(), 640 - 24));
	CHECK(l.tileW > 100 && l.tileH > 80); // room for a 38 px icon, name and count stacked
	// Tiles never overlap each other or the column.
	for (int i = 0; i < 6; i++) {
		Box t{l.grid.x + (i % 2) * (l.tileW + kTileGap), l.grid.y + (i / 2) * (l.tileH + kTileGap), l.tileW, l.tileH};
		CHECK(t.right() <= l.grid.right() + 0.001f && t.bottom() <= l.grid.bottom() + 0.001f);
		for (const Box& b : items) CHECK(!overlaps(t, b));
	}
}

int main() {
	testWide();
	testNarrow();
	if (failures) { std::printf("%d layout test failure(s)\n", failures); return 1; }
	std::printf("layout_test: all passed\n");
	return 0;
}
