// Pixel tests for the rounded-rect helpers behind every button (Gfx::glossyRoundRect, fillRoundRect,
// gradientRoundRect). Draws into an SDL software renderer, so it runs headless on the host; the 16:9 squeeze of the
// Wii (sx = 640 / 854) is tested through Gfx::attachForTest.
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gfx/Gfx.h"
#include "gfx/Theme.h"

static int failures = 0;
#define CHECK(c)                                                                  \
	do {                                                                          \
		if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
	} while (0)

namespace {
constexpr int kW = 320, kH = 160;
constexpr Color kBg{255, 0, 255};

struct Canvas {
	SDL_Surface* s = nullptr;
	SDL_Renderer* r = nullptr;
	Gfx g;
	explicit Canvas(float sx) {
		s = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_RGBA32);
		r = SDL_CreateSoftwareRenderer(s);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		g.attachForTest(r, kW, kH, sx);
		clear();
	}
	~Canvas() {
		g.shutdown(); // frees the cached corner masks and the renderer
		SDL_FreeSurface(s);
	}
	void clear() {
		SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, 255);
		SDL_RenderClear(r);
	}
	Color at(int x, int y) const {
		Uint8 c[4];
		SDL_GetRGBA(((const Uint32*)((const Uint8*)s->pixels + y * s->pitch))[x], s->format, &c[0], &c[1], &c[2], &c[3]);
		return {c[0], c[1], c[2]};
	}
	bool bg(int x, int y) const { Color c = at(x, y); return c.r == kBg.r && c.g == kBg.g && c.b == kBg.b; }
	static bool same(Color a, Color b) { return std::abs(a.r - b.r) <= 2 && std::abs(a.g - b.g) <= 2 && std::abs(a.b - b.b) <= 2; }

	// Pixels outside [x0, x1) x [y0, y1) that were drawn on.
	int strays(int x0, int y0, int x1, int y1) const {
		int n = 0;
		for (int y = 0; y < kH; y++)
			for (int x = 0; x < kW; x++)
				if ((x < x0 || x >= x1 || y < y0 || y >= y1) && !bg(x, y)) n++;
		return n;
	}
	// Length of the run of `c` starting at (x, y) and stepping by (dx, dy).
	int run(int x, int y, int dx, int dy, Color c) const {
		int n = 0;
		while (x >= 0 && x < kW && y >= 0 && y < kH && same(at(x, y), c)) { n++; x += dx; y += dy; }
		return n;
	}
};

// Output-pixel bounds of a layout rect, as Gfx snaps them.
struct Px { int x0, y0, x1, y1; };
Px snapped(Rect rc, float sx) {
	return {(int)std::lround(rc.x * sx), (int)std::lround(rc.y), (int)std::lround(rc.right() * sx),
	        (int)std::lround(rc.bottom())};
}

// The outline must be equally thick on the left and right (and top and bottom) wherever the rect lands, including
// fractional positions such as the 2 px hover growth or the 16:9 squeeze.
void testOutlineSymmetry(float sx) {
	const theme::Gloss* presets[] = {&theme::glossNormal, &theme::glossPrimary, &theme::glossSelected,
	                                 &theme::glossDestructive};
	const float offsets[] = {0.0f, 0.25f, 0.5f, 0.75f, 0.3f};
	for (const theme::Gloss* gl : presets) {
		for (float off : offsets) {
			for (float hover : {0.0f, 1.0f}) {
				Canvas c(sx);
				Rect rc{40 + off, 30 + off, 150 + off, 46 + off};
				c.g.glossyRoundRect(rc, 8, *gl, hover);
				Px p = snapped(rc, sx);
				Color border = hover > 0 ? (gl->outlineToWhite ? lerp(gl->border, Color{255, 255, 255}, 0.5f)
				                                               : lerp(gl->border, theme::accent, 1.0f))
				                         : gl->border;
				int midY = (p.y0 + p.y1) / 2, midX = (p.x0 + p.x1) / 2;
				int left = c.run(p.x0, midY, 1, 0, border), right = c.run(p.x1 - 1, midY, -1, 0, border);
				int top = c.run(midX, p.y0, 0, 1, border), bottom = c.run(midX, p.y1 - 1, 0, -1, border);
				int wantY = (int)std::lround(1.5f + 1.5f * hover);
				int wantX = std::max(1, (int)std::lround((1.5f + 1.5f * hover) * sx));
				if (left != right || left != wantX || top != wantY || bottom != wantY)
					std::printf("  sx=%.2f off=%.2f hover=%.0f: left %d right %d top %d bottom %d (want %d / %d)\n", sx,
					            off, hover, left, right, top, bottom, wantX, wantY);
				CHECK(left == right);
				CHECK(left == wantX);
				CHECK(top == wantY && bottom == wantY);
				CHECK(c.strays(p.x0, p.y0, p.x1, p.y1) == 0);
			}
		}
	}
}

// Capsules: a radius above h / 2 is clamped, nothing spills outside the rect, and the two ends mirror each other.
void testCapsuleClamp(float sx) {
	for (float radius : {19.0f, 30.0f, 999.0f}) {
		Canvas c(sx);
		Rect rc{30.5f, 40, 120, 38};
		c.g.glossyRoundRect(rc, radius, theme::glossPrimary, 0.5f);
		Px p = snapped(rc, sx);
		CHECK(c.strays(p.x0, p.y0, p.x1, p.y1) == 0);
		// The corners are transparent: the first pixel of the top row is background on both ends.
		CHECK(c.bg(p.x0, p.y0) && c.bg(p.x1 - 1, p.y0));
		for (int y = p.y0; y < p.y1; y++) {
			int l = 0, r = 0;
			while (p.x0 + l < p.x1 && c.bg(p.x0 + l, y)) l++;
			while (p.x1 - 1 - r >= p.x0 && c.bg(p.x1 - 1 - r, y)) r++;
			if (std::abs(l - r) > 1) std::printf("  sx=%.2f radius=%.0f row %d: left gap %d right gap %d\n", sx, radius, y, l, r);
			CHECK(std::abs(l - r) <= 1);
		}
	}
}

// Degenerate rects must not crash or draw anywhere they shouldn't.
void testTinyRects(float sx) {
	const Rect rects[] = {{50, 50, 0, 0}, {50, 50, 0, 20}, {50, 50, 20, 0}, {50, 50, -5, 10}, {50, 50, 1, 1},
	                      {50, 50, 2, 2}, {50, 50, 3, 3}, {50.5f, 50.5f, 4.2f, 3.7f}, {50, 50, 6, 6}};
	for (const Rect& rc : rects) {
		for (float radius : {0.0f, 1.0f, 8.0f, -3.0f}) {
			Canvas c(sx);
			c.g.glossyRoundRect(rc, radius, theme::glossNormal, 1.0f);
			c.g.fillRoundRect(rc, radius, Color{0, 0, 0});
			const Gfx::GradStop st[2] = {{0, Color{0, 0, 0}}, {1, Color{255, 255, 255}}};
			c.g.gradientRoundRect(rc, radius, st, 2);
			if (rc.w <= 0 || rc.h <= 0) {
				CHECK(c.strays(kW, kH, kW, kH) == 0); // nothing at all
			} else {
				Px p = snapped(rc, sx);
				CHECK(c.strays(p.x0, p.y0, p.x1, p.y1) == 0);
			}
		}
	}
}

// fillRoundRect and gradientRoundRect round the same rect the same way, so a body drawn over its own shape covers
// it exactly (the glossy body relies on this to sit evenly inside the outline).
void testFillMatchesGradient(float sx) {
	for (float radius : {6.0f, 10.5f, 23.0f}) {
		Canvas a(sx), b(sx);
		Rect rc{20.4f, 20.6f, 90.3f, 46.2f};
		a.g.fillRoundRect(rc, radius, Color{0, 0, 0});
		const Gfx::GradStop st[2] = {{0, Color{0, 0, 0}}, {1, Color{0, 0, 0}}};
		b.g.gradientRoundRect(rc, radius, st, 2);
		Px p = snapped(rc, sx);
		CHECK(a.strays(p.x0, p.y0, p.x1, p.y1) == 0 && b.strays(p.x0, p.y0, p.x1, p.y1) == 0);
		// The straight middle rows match exactly; the corners differ only by the mask's anti-aliasing.
		int midY = (p.y0 + p.y1) / 2;
		for (int x = 0; x < kW; x++) CHECK(a.bg(x, midY) == b.bg(x, midY));
	}
}
} // namespace

int main() {
	setenv("SDL_VIDEODRIVER", "dummy", 0);
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		std::printf("SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	for (float sx : {1.0f, 640.0f / 854.0f}) {
		testOutlineSymmetry(sx);
		testCapsuleClamp(sx);
		testTinyRects(sx);
		testFillMatchesGradient(sx);
	}
	SDL_Quit();
	if (failures) {
		std::printf("gfx_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("gfx_test: all passed\n");
	return 0;
}
