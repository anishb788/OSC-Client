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

void Gfx::drawTexture(SDL_Texture* tex, Rect dst, Color tint, const SDL_Rect* src) {
	if (!tex) return;
	SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
	SDL_SetTextureAlphaMod(tex, tint.a);
	SDL_FRect d = map(dst);
	SDL_RenderCopyF(r, tex, src, &d);
}

void Gfx::fillRoundRect(Rect rc, float radius, Color c) {
	if (rc.w <= 0 || rc.h <= 0) return;
	int rad = (int)std::lround(std::min({radius, rc.w / 2.0f, rc.h / 2.0f}));
	if (rad <= 0) { fillRect(rc, c); return; }

	// Work in output pixels, snapped, so the pieces meet without seams.
	int x0 = (int)std::lround(rc.x * sx), x1 = (int)std::lround(rc.right() * sx);
	int y0 = (int)std::lround(rc.y), y1 = (int)std::lround(rc.bottom());
	int rx = std::min((int)std::lround(rad * sx), (x1 - x0) / 2), ry = std::min(rad, (y1 - y0) / 2);
	if (rx <= 0 || ry <= 0) { fillRect(rc, c); return; }

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
