#include "gfx/Gfx.h"

#include <cmath>

#include "core/Log.h"
#include "gfx/Theme.h"

namespace {

struct V2 { float x, y; };

float segDist(float px, float py, V2 a, V2 b) {
	float ex = b.x - a.x, ey = b.y - a.y, wx = px - a.x, wy = py - a.y;
	float t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
	float dx = wx - ex * t, dy = wy - ey * t;
	return std::sqrt(dx * dx + dy * dy);
}

// Signed distance to a polygon (Inigo Quilez), negative inside.
float polyDist(float px, float py, const V2* v, int n) {
	float d = 1e9f, s = 1.0f;
	for (int i = 0, j = n - 1; i < n; j = i, i++) {
		d = std::min(d, segDist(px, py, v[i], v[j]));
		float ex = v[j].x - v[i].x, ey = v[j].y - v[i].y, wx = px - v[i].x, wy = py - v[i].y;
		bool c1 = py >= v[i].y, c2 = py < v[j].y, c3 = ex * wy > ey * wx;
		if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
	}
	return s * d;
}

// Signed distance to a rounded box (centre, half sizes, corner radius), negative inside.
float boxDist(float px, float py, float cx, float cy, float hx, float hy, float r) {
	float qx = std::fabs(px - cx) - hx + r, qy = std::fabs(py - cy) - hy + r;
	return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

// Arrow pointer, tip at (3,3) in a 30x38 box.
constexpr V2 kCursor[] = {{4, 4}, {4, 29}, {10.5f, 23.5f}, {15, 33}, {20.5f, 30.5f}, {16, 21.5f}, {24.5f, 21.5f}};
constexpr int kCursorW = 30, kCursorH = 38;


} // namespace

bool Gfx::init(bool widescreen, int pcWindowScale) {
	W = widescreen ? 854 : 640;
	H = 480;

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
#ifdef __wii__
	(void)pcWindowScale;
	win = SDL_CreateWindow("OpenShop", 0, 0, 640, 480, 0);
#else
	win = SDL_CreateWindow("OpenShop", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                       W * pcWindowScale, H * pcWindowScale, SDL_WINDOW_RESIZABLE);
#endif
	if (!win) { LOG("SDL_CreateWindow: %s", SDL_GetError()); return false; }

	r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!r) r = SDL_CreateRenderer(win, -1, 0);
	if (!r) { LOG("SDL_CreateRenderer: %s", SDL_GetError()); return false; }

	SDL_RendererInfo info;
	SDL_GetRendererInfo(r, &info);
	LOG("Renderer: %s, layout %dx%d", info.name, W, H);

#ifdef __wii__
	// The Wii always outputs 640x480; in 16:9 the TV stretches it horizontally,
	// so lay out at 854 wide and squeeze. Done by hand in map(): the GX renderer
	// does not support non-uniform SDL_RenderSetScale.
	sx = 640.0f / W;
#else
	SDL_RenderSetLogicalSize(r, W, H);
#endif
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_ShowCursor(SDL_DISABLE);

	cursorEdge = maskTexture(kCursorW, kCursorH, [](float x, float y) {
		return polyDist(x, y, kCursor, 7) - 3.0f;
	});
	cursorFill = maskTexture(kCursorW, kCursorH, [](float x, float y) {
		return polyDist(x, y, kCursor, 7) - 0.5f;
	});

	// Background: vertical gradient with faint horizontal pinstripes, baked once.
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
	for (int y = 0; y < H; y++) {
		Color c = lerp(theme::bgTop, theme::bgBottom, (float)y / H);
		if (y % 4 < 2) c = lerp(c, theme::stripe.withAlpha(1), theme::stripe.a / 255.0f);
		Uint32 px = SDL_MapRGBA(s->format, c.r, c.g, c.b, 255);
		Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
		for (int x = 0; x < W; x++) row[x] = px;
	}
	bgTex = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	return true;
}

void Gfx::shutdown() {
	for (auto& [k, t] : circles) SDL_DestroyTexture(t);
	for (auto& [k, t] : icons) SDL_DestroyTexture(t);
	circles.clear();
	icons.clear();
	for (SDL_Texture* t : {bgTex, cursorFill, cursorEdge}) if (t) SDL_DestroyTexture(t);
	if (r) SDL_DestroyRenderer(r);
	if (win) SDL_DestroyWindow(win);
	r = nullptr;
	win = nullptr;
}

void Gfx::beginFrame() {
	SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
	SDL_RenderClear(r);
}

void Gfx::endFrame() { SDL_RenderPresent(r); }

void Gfx::background() { drawTexture(bgTex, {0, 0, (float)W, (float)H}); }

void Gfx::fillRect(Rect rc, Color c) {
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
	SDL_FRect f = map(rc);
	SDL_RenderFillRectF(r, &f);
}

void Gfx::gradientV(Rect rc, Color top, Color bottom) {
	SDL_Vertex v[4] = {
		{{rc.x * sx, rc.y}, {top.r, top.g, top.b, top.a}, {0, 0}},
		{{rc.right() * sx, rc.y}, {top.r, top.g, top.b, top.a}, {0, 0}},
		{{rc.right() * sx, rc.bottom()}, {bottom.r, bottom.g, bottom.b, bottom.a}, {0, 0}},
		{{rc.x * sx, rc.bottom()}, {bottom.r, bottom.g, bottom.b, bottom.a}, {0, 0}},
	};
	const int idx[6] = {0, 1, 2, 0, 2, 3};
	SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
}

SDL_Texture* Gfx::maskTexture(int w, int h, const std::function<float(float, float)>& sdf) {
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
	if (!s) return nullptr;
	for (int y = 0; y < h; y++) {
		Uint32* row = (Uint32*)((Uint8*)s->pixels + y * s->pitch);
		for (int x = 0; x < w; x++) {
			float a = std::clamp(0.5f - sdf(x + 0.5f, y + 0.5f), 0.0f, 1.0f);
			row[x] = SDL_MapRGBA(s->format, 255, 255, 255, (Uint8)(a * 255 + 0.5f));
		}
	}
	SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
	masks++;
	SDL_FreeSurface(s);
	if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	return t;
}

SDL_Texture* Gfx::circleTexture(int radius) {
	auto it = circles.find(radius);
	if (it != circles.end()) return it->second;
	float rr = (float)radius;
	SDL_Texture* t = maskTexture(radius * 2, radius * 2, [rr](float x, float y) {
		return std::sqrt((x - rr) * (x - rr) + (y - rr) * (y - rr)) - rr;
	});
	circles[radius] = t;
	return t;
}

void Gfx::prewarmCircles(int maxRadius) {
	for (int i = 1; i <= maxRadius; i++) circleTexture(i);
}

void Gfx::drawTexture(SDL_Texture* tex, Rect dst, Color tint, const SDL_Rect* src) {
	if (!tex) return;
	SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
	SDL_SetTextureAlphaMod(tex, tint.a);
	SDL_FRect d = map(dst);
	SDL_RenderCopyF(r, tex, src, &d);
}

// Rounded-rect geometry in output pixels, shared by fillRoundRect and gradientRoundRect so a body drawn over an outline
// rounds exactly like the outline does: edges snapped with lround, the radius clamped to half the size and rounded
// to whole pixels (horizontally after the squeeze).
Gfx::Snapped Gfx::snap(Rect rc, float radius) const {
	Snapped g;
	g.x0 = (int)std::lround(rc.x * sx);
	g.x1 = (int)std::lround(rc.right() * sx);
	g.y0 = (int)std::lround(rc.y);
	g.y1 = (int)std::lround(rc.bottom());
	g.rad = (int)std::lround(std::max(0.0f, std::min({radius, rc.w / 2.0f, rc.h / 2.0f})));
	g.rx = std::min((int)std::lround(g.rad * sx), (g.x1 - g.x0) / 2);
	g.ry = std::min(g.rad, (g.y1 - g.y0) / 2);
	return g;
}

void Gfx::fillRoundRect(Rect rc, float radius, Color c) {
	if (rc.w <= 0 || rc.h <= 0) return;
	// Work in output pixels, snapped, so the pieces meet without seams.
	const Snapped g = snap(rc, radius);
	const int x0 = g.x0, x1 = g.x1, y0 = g.y0, y1 = g.y1, rx = g.rx, ry = g.ry, rad = g.rad;
	if (x1 <= x0 || y1 <= y0) return;
	if (rad <= 0 || rx <= 0 || ry <= 0) {
		SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
		SDL_Rect all{x0, y0, x1 - x0, y1 - y0};
		SDL_RenderFillRect(r, &all);
		return;
	}

	SDL_Texture* t = circleTexture(rad);
	SDL_SetTextureColorMod(t, c.r, c.g, c.b);
	SDL_SetTextureAlphaMod(t, c.a);
	// Corners are quadrants of the circle texture; the rest is three rects that do not overlap.
	SDL_Rect src[4] = {{0, 0, rad, rad}, {rad, 0, rad, rad}, {0, rad, rad, rad}, {rad, rad, rad, rad}};
	SDL_Rect dst[4] = {{x0, y0, rx, ry}, {x1 - rx, y0, rx, ry}, {x0, y1 - ry, rx, ry}, {x1 - rx, y1 - ry, rx, ry}};
	for (int i = 0; i < 4; i++) SDL_RenderCopy(r, t, &src[i], &dst[i]);

	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
	SDL_Rect mid{x0 + rx, y0, x1 - x0 - 2 * rx, y1 - y0};
	SDL_Rect left{x0, y0 + ry, rx, y1 - y0 - 2 * ry};
	SDL_Rect right{x1 - rx, y0 + ry, rx, y1 - y0 - 2 * ry};
	if (mid.w > 0) SDL_RenderFillRect(r, &mid);
	if (left.h > 0) { SDL_RenderFillRect(r, &left); SDL_RenderFillRect(r, &right); }
}

void Gfx::gradientRoundRect(Rect rc, float radius, const GradStop* stops, int n) {
	if (rc.w <= 0 || rc.h <= 0 || n < 2) return;
	// Work in output pixels like fillRoundRect.
	const Snapped g = snap(rc, radius);
	const float x0 = (float)g.x0, x1 = (float)g.x1, y0 = (float)g.y0, y1 = (float)g.y1;
	const float h = y1 - y0;
	if (x1 <= x0 || h <= 0) return;
	const float rx = (float)g.rx, ry = (float)g.ry;

	auto colourAt = [&](float t) {
		for (int i = 1; i < n; i++)
			if (t <= stops[i].t && stops[i].t > stops[i - 1].t)
				return lerp(stops[i - 1].c, stops[i].c, (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t));
		return stops[n - 1].c;
	};
	// Horizontal inset of the outline at height y (0 in the straight middle part).
	auto insetAt = [&](float y) {
		float d = std::min(y - y0, y1 - y);
		if (d >= ry || ry <= 0) return 0.0f;
		float k = (ry - d) / ry;
		return rx * (1.0f - std::sqrt(std::max(0.0f, 1.0f - k * k)));
	};

	constexpr int kMaxArc = 8, kMaxRows = 2 * (kMaxArc + 1) + 8;
	// Arc steps by size: a 10 px corner needs 4, a 22 px capsule end 8 (fewer triangles on the Wii and in software).
	const int kArc = std::clamp((int)std::ceil(ry / 3.0f), 2, kMaxArc);
	struct Row { float y; Color c; };
	Row rows[kMaxRows];
	int nr = 0;
	auto stopNear = [&](float y) {
		for (int i = 0; i < n; i++) if (std::fabs(y0 + stops[i].t * h - y) < 0.01f) return true;
		return false;
	};
	for (int i = 0; i <= kArc && nr < kMaxRows - 1; i++) {
		float a = 1.5707963f * i / kArc, d = ry * (1.0f - std::cos(a));
		for (float y : {y0 + d, y1 - d}) {
			if (stopNear(y) || nr >= kMaxRows) continue;
			rows[nr++] = {y, colourAt((y - y0) / h)};
		}
	}
	for (int i = 0; i < n && nr < kMaxRows; i++) rows[nr++] = {y0 + stops[i].t * h, stops[i].c};
	// Stable insertion sort by y (stop rows with equal y keep their order).
	for (int i = 1; i < nr; i++) {
		Row k = rows[i];
		int j = i - 1;
		while (j >= 0 && rows[j].y > k.y) { rows[j + 1] = rows[j]; j--; }
		rows[j + 1] = k;
	}

	SDL_Vertex v[kMaxRows * 2];
	int idx[kMaxRows * 6];
	int ni = 0;
	for (int i = 0; i < nr; i++) {
		float in = insetAt(rows[i].y);
		SDL_Color c{rows[i].c.r, rows[i].c.g, rows[i].c.b, rows[i].c.a};
		v[2 * i] = {{x0 + in, rows[i].y}, c, {0, 0}};
		v[2 * i + 1] = {{x1 - in, rows[i].y}, c, {0, 0}};
		if (i > 0 && rows[i].y > rows[i - 1].y) {
			int a = 2 * (i - 1), b = 2 * i;
			const int q[6] = {a, a + 1, b + 1, a, b + 1, b};
			for (int k : q) idx[ni++] = k;
		}
	}
	if (ni > 0) SDL_RenderGeometry(r, nullptr, v, nr * 2, idx, ni);
}

void Gfx::glossyRoundRect(Rect rc, float radius, const theme::Gloss& gl, float hover) {
	if (rc.w <= 0 || rc.h <= 0) return;
	// Snap the outer rect to output pixels and use a whole-pixel outline on every side (horizontally after the 16:9
	// squeeze), so the outline is equally thick all round instead of 2 px on one side and 1 px on the other.
	const Snapped o = snap(rc, radius);
	if (o.x1 <= o.x0 || o.y1 <= o.y0) return;
	const float bw = 1.5f + 1.5f * hover;
	const int ny = std::max(1, (int)std::lround(bw)), nx = std::max(1, (int)std::lround(bw * sx));
	auto layout = [this](int x0, int y0, int x1, int y1) {
		return Rect{x0 / sx, (float)y0, (x1 - x0) / sx, (float)(y1 - y0)};
	};
	const Rect outer = layout(o.x0, o.y0, o.x1, o.y1);
	Color border = gl.outlineToWhite ? lerp(gl.border, Color{255, 255, 255}, 0.5f * hover) : lerp(gl.border, theme::accent, hover);
	fillRoundRect(outer, (float)o.rad, border);

	if (o.x1 - o.x0 <= 2 * nx || o.y1 - o.y0 <= 2 * ny) return;
	const Rect body = layout(o.x0 + nx, o.y0 + ny, o.x1 - nx, o.y1 - ny);
	float amt = gl.hoverAmount * hover;
	Color top = lerp(gl.top, gl.hoverTo, amt), mid = lerp(gl.mid, gl.hoverTo, amt), bot = lerp(gl.bottom, gl.hoverTo, amt);
	// The bottom ~3 px are a little darker (the button's lower edge), as a hard step.
	float edge = std::min(3.0f, body.h * 0.12f);
	Color edgeCol = lerp(bot, Color{0, 40, 80}, 0.08f);
	float te = 1.0f - edge / body.h;
	const GradStop stops[5] = {{0, top}, {0.55f, mid}, {te, lerp(mid, bot, (te - 0.55f) / 0.45f)}, {te, edgeCol}, {1, edgeCol}};
	gradientRoundRect(body, (float)std::max(0, o.rad - ny), stops, 5);

	// Shine band: top 42 %, inset from the sides, fading from gl.shineTop to gl.shineBottom.
	float inx = std::min(10.0f, body.w * 0.2f);
	Rect shine{body.x + inx, body.y + std::min(3.0f, body.h * 0.1f), body.w - 2 * inx, body.h * 0.42f};
	const GradStop sh[2] = {{0, Color{255, 255, 255, gl.shineTop}}, {1, Color{255, 255, 255, gl.shineBottom}}};
	gradientRoundRect(shine, std::min(shine.w, shine.h) / 2, sh, 2);
}

void Gfx::roundRectBorder(Rect rc, float radius, float bw, Color border, Color fill) {
	fillRoundRect(rc, radius, border);
	fillRoundRect(rc.inset(bw), std::max(0.0f, radius - bw), fill);
}

void Gfx::softShadow(Rect rc, float radius, float strength) {
	const struct { float spread, dy, alpha; } layers[] = {{6, 5, 8}, {3, 3, 14}, {1, 1, 22}};
	for (auto& l : layers) {
		Rect s{rc.x - l.spread, rc.y - l.spread + l.dy, rc.w + 2 * l.spread, rc.h + 2 * l.spread};
		fillRoundRect(s, radius + l.spread, Color{0, 40, 80, (Uint8)(l.alpha * strength)});
	}
}

void Gfx::circle(float cx, float cy, float radius, Color c) {
	fillRoundRect({cx - radius, cy - radius, radius * 2, radius * 2}, radius, c);
}

SDL_Texture* Gfx::iconTexture(Icon kind, int size) {
	int key = (int)kind * 1000 + size;
	auto it = icons.find(key);
	if (it != icons.end()) return it->second;

	float s = (float)size;
	auto P = [s](float x, float y) { return V2{x * s, y * s}; };
	std::function<float(float, float)> f;
	float t = 0.085f * s;
	switch (kind) {
	case Icon::ChevronLeft:
	case Icon::ChevronRight:
	case Icon::ChevronUp:
	case Icon::ChevronDown: {
		V2 a = P(0.40f, 0.20f), m = P(0.66f, 0.50f), b = P(0.40f, 0.80f);
		auto rot = [&](V2 v) -> V2 {
			float x = v.x / s - 0.5f, y = v.y / s - 0.5f;
			switch (kind) {
			case Icon::ChevronLeft: return P(0.5f - x + 0.03f, 0.5f + y);
			case Icon::ChevronUp: return P(0.5f + y, 0.5f - x + 0.03f);
			case Icon::ChevronDown: return P(0.5f + y, 0.5f + x - 0.03f);
			default: return P(0.5f + x - 0.03f, 0.5f + y);
			}
		};
		a = rot(a); m = rot(m); b = rot(b);
		f = [=](float x, float y) { return std::min(segDist(x, y, a, m), segDist(x, y, m, b)) - t * 1.2f; };
		break;
	}
	case Icon::Search: {
		V2 c = P(0.42f, 0.42f), h0 = P(0.62f, 0.62f), h1 = P(0.84f, 0.84f);
		float R = 0.25f * s;
		f = [=](float x, float y) {
			float ring = std::fabs(std::hypot(x - c.x, y - c.y) - R) - t;
			return std::min(ring, segDist(x, y, h0, h1) - t * 1.3f);
		};
		break;
	}
	case Icon::Download: {
		V2 a = P(0.5f, 0.14f), b = P(0.5f, 0.62f), l = P(0.28f, 0.42f), rr = P(0.72f, 0.42f);
		V2 b0 = P(0.18f, 0.86f), b1 = P(0.82f, 0.86f);
		f = [=](float x, float y) {
			float d = std::min({segDist(x, y, a, b), segDist(x, y, l, b), segDist(x, y, rr, b)});
			return std::min(d, segDist(x, y, b0, b1)) - t;
		};
		break;
	}
	case Icon::Warning: {
		V2 tri[3] = {P(0.5f, 0.12f), P(0.92f, 0.86f), P(0.08f, 0.86f)};
		V2 e0 = P(0.5f, 0.38f), e1 = P(0.5f, 0.6f), dot = P(0.5f, 0.74f);
		f = [=](float x, float y) {
			float body = polyDist(x, y, tri, 3) - 0.04f * s;
			float mark = std::min(segDist(x, y, e0, e1) - 0.06f * s, std::hypot(x - dot.x, y - dot.y) - 0.065f * s);
			return std::max(body, -mark);
		};
		break;
	}
	case Icon::Note: {
		V2 head = P(0.36f, 0.72f), s0 = P(0.5f, 0.72f), s1 = P(0.5f, 0.16f), fl = P(0.76f, 0.3f);
		f = [=](float x, float y) {
			float d = std::hypot(x - head.x, y - head.y) - 0.15f * s;
			d = std::min(d, segDist(x, y, s0, s1) - t * 0.7f);
			return std::min(d, segDist(x, y, s1, fl) - t * 0.8f);
		};
		break;
	}
	case Icon::Close: {
		V2 a = P(0.25f, 0.25f), b = P(0.75f, 0.75f), c = P(0.75f, 0.25f), d = P(0.25f, 0.75f);
		f = [=](float x, float y) { return std::min(segDist(x, y, a, b), segDist(x, y, c, d)) - t; };
		break;
	}
	case Icon::Refresh: {
		// Open circular arrow: a clockwise arc with a gap at the upper right, ending in an arrowhead.
		V2 c = P(0.5f, 0.5f);
		float R = 0.32f * s;
		const float kPi = 3.14159265f;
		float gapA = -75.0f * kPi / 180, gapB = -15.0f * kPi / 180; // screen angles, y down, so clockwise = increasing
		V2 e{c.x + R * std::cos(gapA), c.y + R * std::sin(gapA)};
		V2 tg{-std::sin(gapA), std::cos(gapA)}, nr{std::cos(gapA), std::sin(gapA)};
		float hw = 0.2f * s, hl = 0.26f * s;
		V2 tri[3] = {{e.x + tg.x * hl, e.y + tg.y * hl}, {e.x + nr.x * hw, e.y + nr.y * hw}, {e.x - nr.x * hw, e.y - nr.y * hw}};
		f = [=](float x, float y) {
			float a = std::atan2(y - c.y, x - c.x);
			float arc = std::fabs(std::hypot(x - c.x, y - c.y) - R) - t * 1.1f;
			if (a > gapA - 0.02f && a < gapB) {
				// Inside the gap: the arc ends are round caps.
				V2 s0{c.x + R * std::cos(gapB), c.y + R * std::sin(gapB)};
				arc = std::min(std::hypot(x - s0.x, y - s0.y), std::hypot(x - e.x, y - e.y)) - t * 1.1f;
			}
			return std::min(arc, polyDist(x, y, tri, 3));
		};
		break;
	}
	case Icon::Gear: {
		// Toothed ring with a centre hole: 6 chunky, square-ish teeth (steep flanks), one pointing straight up. Wide
		// teeth and a big hole keep it reading as a gear at ~20 px and through the Wii's 16:9 squeeze.
		V2 c = P(0.5f, 0.5f);
		float body = 0.33f * s, tooth = 0.46f * s, hole = 0.16f * s;
		f = [=](float x, float y) {
			float dx = x - c.x, dy = y - c.y, d = std::hypot(dx, dy);
			float a = std::atan2(dy, dx) + 1.5707963f; // 0 at "up" (screen y grows downward)
			float k = std::clamp((std::cos(6.0f * a) + 0.2f) * 8.0f, 0.0f, 1.0f);
			float outer = d - (body + (tooth - body) * k);
			return std::max(outer, hole - d);
		};
		break;
	}
		case Icon::Package: {
			// Open box (hexagon outline with the three inner edges), stroke style.
			const float u = s / 24, hw = 1.15f * u;
			auto Q = [u](float gx, float gy) { return V2{gx * u, gy * u}; };
			V2 hex[6] = {Q(3.5f, 7.5f), Q(12, 3), Q(20.5f, 7.5f), Q(20.5f, 16.5f), Q(12, 21), Q(3.5f, 16.5f)};
			V2 mid = Q(12, 12), l = Q(3.5f, 7.5f), rr = Q(20.5f, 7.5f), bot = Q(12, 21);
			f = [=](float x, float y) {
				float d = std::min({segDist(x, y, mid, l), segDist(x, y, mid, rr), segDist(x, y, mid, bot)});
				for (int i = 0, j = 5; i < 6; j = i, i++) d = std::min(d, segDist(x, y, hex[i], hex[j]));
				return d - hw;
			};
			break;
		}
		case Icon::Gamepad: {
			const float u = s / 24;
			f = [=](float x, float y) {
				float px = x / u, py = y / u;
				float d = boxDist(px, py, 12, 11.8f, 9.6f, 4.6f, 4.4f);
				d = std::min({d, std::hypot(px - 5.9f, py - 15.9f) - 2.6f, std::hypot(px - 18.1f, py - 15.9f) - 2.6f});
				float cut = std::min(boxDist(px, py, 7.1f, 12, 2.0f, 0.7f, 0.3f), boxDist(px, py, 7.1f, 12, 0.7f, 2.0f, 0.3f));
				cut = std::min({cut, std::hypot(px - 16.2f, py - 10.9f) - 1.05f, std::hypot(px - 18.0f, py - 12.8f) - 1.05f});
				return std::max(d, -cut) * u;
			};
			break;
		}
		case Icon::Cartridge: {
			const float u = s / 24;
			auto Q = [u](float gx, float gy) { return V2{gx * u, gy * u}; };
			// The outline inset by 1.3, then grown back by 1.3: rounded corners.
			V2 body[6] = {Q(6.3f, 4.3f), Q(17.7f, 4.3f), Q(17.7f, 18.46f), Q(16.46f, 19.7f), Q(7.54f, 19.7f), Q(6.3f, 18.46f)};
			V2 pin[4][2] = {{Q(8.6f, 17), Q(8.6f, 19.2f)}, {Q(10.9f, 17), Q(10.9f, 19.2f)}, {Q(13.1f, 17), Q(13.1f, 19.2f)},
			                {Q(15.4f, 17), Q(15.4f, 19.2f)}};
			f = [=](float x, float y) {
				float d = polyDist(x, y, body, 6) - 1.3f * u;
				float cut = boxDist(x / u, y / u, 12, 9.1f, 4.0f, 3.5f, 1.2f) * u;
				for (auto& p : pin) cut = std::min(cut, segDist(x, y, p[0], p[1]) - 0.65f * u);
				return std::max(d, -cut);
			};
			break;
		}
		case Icon::Wrench: {
			const float u = s / 24;
			V2 head = {15.2f * u, 8.8f * u}, jaw = {21.5f * u, 2.5f * u}, h0 = {6.4f * u, 17.6f * u}, h1 = {12.6f * u, 11.4f * u};
			f = [=](float x, float y) {
				float d = std::min(std::hypot(x - head.x, y - head.y) - 5.2f * u, segDist(x, y, h0, h1) - 2.3f * u);
				float notch = segDist(x, y, head, jaw) - 1.9f * u;
				return std::max(d, -notch);
			};
			break;
		}
		case Icon::Play: {
			const float u = s / 24, r = 1.3f * u;
			V2 t[3] = {{8.2f * u, 3.8f * u}, {8.2f * u, 20.2f * u}, {21.0f * u, 12.0f * u}};
			// Round the corners: shrink the triangle about its incentre so its edges move in by r, then grow by r.
			float a = std::hypot(t[1].x - t[2].x, t[1].y - t[2].y), b = std::hypot(t[0].x - t[2].x, t[0].y - t[2].y),
			      c = std::hypot(t[0].x - t[1].x, t[0].y - t[1].y), per = a + b + c;
			V2 inc = {(a * t[0].x + b * t[1].x + c * t[2].x) / per, (a * t[0].y + b * t[1].y + c * t[2].y) / per};
			float area = std::fabs((t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y)) / 2;
			float k = 1.0f - r / (area / (per / 2));
			for (V2& v : t) v = {inc.x + (v.x - inc.x) * k, inc.y + (v.y - inc.y) * k};
			f = [=](float x, float y) { return polyDist(x, y, t, 3) - r; };
			break;
		}
		case Icon::Sparkle: {
			const float u = s / 24;
			auto Q = [u](float gx, float gy) { return V2{gx * u, gy * u}; };
			V2 big[8] = {Q(11, 2.8f), Q(13.1f, 8.5f), Q(18.8f, 10.6f), Q(13.1f, 12.7f), Q(11, 18.4f), Q(8.9f, 12.7f), Q(3.2f, 10.6f), Q(8.9f, 8.5f)};
			V2 small[8] = {Q(18.3f, 14.6f), Q(19.2f, 16.9f), Q(21.5f, 17.8f), Q(19.2f, 18.7f), Q(18.3f, 21), Q(17.4f, 18.7f), Q(15.1f, 17.8f), Q(17.4f, 16.9f)};
			f = [=](float x, float y) { return std::min(polyDist(x, y, big, 8), polyDist(x, y, small, 8)) - 0.25f * u; };
			break;
		}
		case Icon::Grid: {
			const float u = s / 24;
			f = [=](float x, float y) {
				float px = x / u, py = y / u, d = 1e9f;
				for (float cx : {7.0f, 17.0f}) for (float cy : {7.0f, 17.0f}) d = std::min(d, boxDist(px, py, cx, cy, 4, 4, 2.2f));
				return d * u;
			};
			break;
		}
	}
	SDL_Texture* tex = maskTexture(size, size, f);
	icons[key] = tex;
	return tex;
}

void Gfx::icon(Icon kind, Rect rc, Color c) {
	int size = (int)std::lround(std::min(rc.w, rc.h));
	if (size <= 0) return;
	Rect d{std::round(rc.cx() - size / 2.0f), std::round(rc.cy() - size / 2.0f), (float)size, (float)size};
	drawTexture(iconTexture(kind, size), d, c);
}

void Gfx::cursor(float x, float y) {
	Rect d{std::round(x - 4), std::round(y - 4), (float)kCursorW, (float)kCursorH};
	drawTexture(cursorEdge, d.offset(1, 2), Color{0, 30, 60, 60});
	drawTexture(cursorEdge, d, theme::white);
	drawTexture(cursorFill, d, theme::accent);
}

void Gfx::spinner(float cx, float cy, float radius, double time, Color c) {
	const int n = 10;
	float head = (float)std::fmod(time * 1.4, 1.0);
	for (int i = 0; i < n; i++) {
		float a = (float)i / n * 6.2831853f - 1.5707963f;
		float age = std::fmod(head - (float)i / n + 1.0f, 1.0f);
		float k = 1.0f - age;
		float dotR = radius * (0.12f + 0.06f * k);
		circle(cx + std::cos(a) * radius, cy + std::sin(a) * radius, dotR, c.withAlpha(0.2f + 0.8f * k * k));
	}
}

void Gfx::pushClip(Rect rc) {
	SDL_FRect f = map(rc);
	SDL_Rect c{(int)std::lround(f.x), (int)std::lround(f.y), (int)std::lround(f.w), (int)std::lround(f.h)};
	if (!clips.empty()) SDL_IntersectRect(&clips.back(), &c, &c);
	clips.push_back(c);
	SDL_RenderSetClipRect(r, &c);
}

void Gfx::popClip() {
	clips.pop_back();
	SDL_RenderSetClipRect(r, clips.empty() ? nullptr : &clips.back());
}
