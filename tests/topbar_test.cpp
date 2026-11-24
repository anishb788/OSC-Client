// Host tests for the pure top-bar layout (no SDL): capsule rects, collapse order, titleRight, subtitle rule.
#include <cmath>
#include <cstdio>

#include "ui/TopBarLayout.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

using namespace topbar;

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
static const float kMargin = 24;

static void testNoneVisible() {
	for (float W : {640.0f, 854.0f}) {
		Layout l = layout(W, kMargin, {false, false, false});
		CHECK(l.count == 0);
		CHECK(near(l.titleRight, W - kMargin)); // today's limit
	}
}

static void testRects() {
	// All three at 640 (4:3, the primary layout): the last right edge is W - margin, 8 px between capsules.
	Layout l = layout(640, kMargin, {true, true, true});
	CHECK(l.count == 3);
	CHECK(near(l.rect[2].x + l.rect[2].w, 640 - kMargin));
	CHECK(near(l.rect[1].x + l.rect[1].w + kGap, l.rect[2].x));
	CHECK(near(l.rect[0].x + l.rect[0].w + kGap, l.rect[1].x));
	CHECK(l.rect[0].x < l.rect[1].x && l.rect[1].x < l.rect[2].x); // Downloads, Refresh, Settings
	for (int i = 0; i < 3; i++) CHECK(near(l.rect[i].w, kCapW) && near(l.rect[i].h, kCapH) && near(l.rect[i].y, kTop));
	CHECK(near(l.titleRight, l.rect[0].x - kTitleGap));
	CHECK(near(l.rect[0].x, 640 - 24 - 3 * 52 - 2 * 8));

	// 854 (16:9) anchors to its own right edge.
	Layout w = layout(854, kMargin, {true, true, true});
	CHECK(near(w.rect[2].x + w.rect[2].w, 854 - kMargin));
	CHECK(near(w.rect[0].x, 854 - 24 - 3 * 52 - 2 * 8));
}

static void testCollapse() {
	// Only Refresh (the M2 end state): it takes the rightmost slot.
	Layout r = layout(640, kMargin, {false, true, false});
	CHECK(r.count == 1 && near(r.rect[1].x + r.rect[1].w, 640 - kMargin));
	CHECK(near(r.titleRight, r.rect[1].x - kTitleGap));

	// Refresh and Settings (M4 added): Refresh moves one slot left, Settings is rightmost.
	Layout rs = layout(640, kMargin, {false, true, true});
	CHECK(rs.count == 2);
	CHECK(near(rs.rect[2].x + rs.rect[2].w, 640 - kMargin));
	CHECK(near(rs.rect[1].x + rs.rect[1].w + kGap, rs.rect[2].x));
	CHECK(near(rs.rect[1].x, r.rect[1].x - kCapW - kGap));

	// Downloads and Settings with Refresh hidden: no gap where Refresh would be.
	Layout ds = layout(640, kMargin, {true, false, true});
	CHECK(near(ds.rect[0].x + ds.rect[0].w + kGap, ds.rect[2].x));
	CHECK(!ds.visible[1]);

	// Titles stop further left the more buttons there are.
	CHECK(r.titleRight > rs.titleRight);
	CHECK(rs.titleRight > layout(640, kMargin, {true, true, true}).titleRight);
}

static void testFit() {
	// Explicit numbers (margin 24). At 640 with three capsules: x = 616 - 3*52 - 2*8 = 444, titleRight = 430,
	// so 406 px are available; kTitleFrac * 640 = 396.8.
	Layout all = layout(640, kMargin, {true, true, true});
	CHECK(near(all.titleRight, 430));

	// Short title, normal subtitle: room = 406 - 100 - 14 = 292 >= min(150, 100). Title capped at 396.8.
	Fit f = fit(640, kMargin, 430, 100, 150);
	CHECK(f.showSubtitle && near(f.titleMaxW, 396.8f));

	// Exactly at the drop threshold: subtitle 200 needs 100 px; title 292 leaves 406 - 292 - 14 = 100 -> kept.
	f = fit(640, kMargin, 430, 292, 200);
	CHECK(f.showSubtitle);
	// Half a pixel more title: 99.5 px left -> dropped, and the title may use all 406 px.
	f = fit(640, kMargin, 430, 292.5f, 200);
	CHECK(!f.showSubtitle && near(f.titleMaxW, 406));

	// A short subtitle (40 px) needs only its own width: title 330 leaves 62 px -> kept.
	f = fit(640, kMargin, 430, 330, 40);
	CHECK(f.showSubtitle);
	f = fit(640, kMargin, 430, 353, 40); // 406 - 353 - 14 = 39 < 40 -> dropped
	CHECK(!f.showSubtitle);

	// Title wider than the cap: capped at 396.8, leaving -4.8 px -> subtitle dropped, title gets 406.
	f = fit(640, kMargin, 430, 900, 300);
	CHECK(!f.showSubtitle && near(f.titleMaxW, 406));

	// No subtitle at all: the title may use everything up to titleRight.
	f = fit(640, kMargin, 430, 100, 0);
	CHECK(!f.showSubtitle && near(f.titleMaxW, 406));

	// No buttons at 640: titleRight 616, 592 px available; a long title is still capped at 396.8 and the
	// subtitle keeps 592 - 396.8 - 14 = 181.2 px.
	f = fit(640, kMargin, 616, 900, 200);
	CHECK(f.showSubtitle && near(f.titleMaxW, 396.8f));

	// 16:9 (854) with three capsules: titleRight = 830 - 172 - 14 = 644, 620 px available, cap 529.48.
	Layout wide = layout(854, kMargin, {true, true, true});
	CHECK(near(wide.titleRight, 644));
	f = fit(854, kMargin, 644, 600, 200); // 620 - 529.48 - 14 = 76.52 < 100 -> dropped
	CHECK(!f.showSubtitle && near(f.titleMaxW, 620));
	f = fit(854, kMargin, 644, 300, 200); // 620 - 300 - 14 = 306 -> kept
	CHECK(f.showSubtitle && near(f.titleMaxW, 529.48f));
}

int main() {
	testNoneVisible();
	testRects();
	testCollapse();
	testFit();
	if (failures) { std::printf("%d topbar test failure(s)\n", failures); return 1; }
	std::printf("topbar_test: all passed\n");
	return 0;
}
