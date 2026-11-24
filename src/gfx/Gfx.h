#pragma once
#include <functional>
#include <unordered_map>
#include <vector>

#include "core/Types.h"
#include "gfx/Theme.h"

enum class Icon { ChevronLeft, ChevronRight, ChevronUp, ChevronDown, Search, Download, Warning, Note, Close, Refresh, Gear,
	// Package is a stroke icon (My apps); the rest are filled category pictograms drawn in the accent blue on Home.
	Package, Gamepad, Cartridge, Wrench, Play, Sparkle, Grid };

class Gfx {
public:
	bool init(bool widescreen, int pcWindowScale);
	void shutdown();

	void beginFrame();
	void endFrame();

	int width() const { return W; }
	// Horizontal squeeze from layout to output (854 -> 640 for anamorphic 16:9 on Wii).
	float scaleX() const { return sx; }
	int height() const { return H; }
	SDL_Renderer* renderer() const { return r; }
	// Textures created by maskTexture (circles, icons, cursor) so far; stays flat once the UI has been drawn.
	int masksCreated() const { return masks; }

	void fillRect(Rect rc, Color c);
	void gradientV(Rect rc, Color top, Color bottom);
	void fillRoundRect(Rect rc, float radius, Color c);
	// Opaque rounded rect with a border of width bw.
	// The one drawing helper behind every button (see theme::Gloss): outline, vertical gradient, bottom edge and shine
	// band inside a rounded rect. `hover` (0..1) eases the colours toward the preset's hover colour and the outline
	// toward the accent (or toward white for presets with outlineToWhite). The outline is a whole number of output
	// pixels on every side. The shadow and focus glow stay with the caller (pillButton). Built from textured corners
	// plus two SDL_RenderGeometry calls: no per-frame texture creation.
	void glossyRoundRect(Rect rc, float radius, const theme::Gloss& gloss, float hover = 0.0f);
	struct GradStop { float t; Color c; };
	// Fills the rounded rect with a vertical gradient through `stops` (t ascending in 0..1; two stops with the same t make
	// a hard step) using one SDL_RenderGeometry call. The edge is not anti-aliased: draw it over a matching border.
	void gradientRoundRect(Rect rc, float radius, const GradStop* stops, int n);
	void roundRectBorder(Rect rc, float radius, float bw, Color border, Color fill);
	void softShadow(Rect rc, float radius, float strength = 1.0f);
	// Round shapes are for decoration only: page-indicator dots and the spinner's dots. Buttons are never circles:
	// every button is a capsule or a rounded rectangle (pillButton / arrowButton), so do not add circular buttons.
	void circle(float cx, float cy, float radius, Color c);
	void icon(Icon kind, Rect rc, Color c);
	void cursor(float x, float y);
	void spinner(float cx, float cy, float radius, double time, Color c);
	void background();

	void drawTexture(SDL_Texture* tex, Rect dst, Color tint = {}, const SDL_Rect* src = nullptr);

	void pushClip(Rect rc);
	void popClip();

	// Creates an anti-aliased alpha mask from a signed distance function
	// (negative inside, in pixels). Result is white; tint when drawing.
	SDL_Texture* maskTexture(int w, int h, const std::function<float(float, float)>& sdf);

	// Test hook (tests/gfx_test.cpp): draw into a caller-owned (software) renderer with a chosen squeeze, without a
	// window. Never called by the app; the caller destroys the renderer itself.
	void attachForTest(SDL_Renderer* renderer, int w, int h, float scale) { r = renderer; W = w; H = h; sx = scale; }

	// Pre-creates the corner masks for radii 1..maxRadius so the first hover of a big capsule does not build a
	// texture mid-frame on the Wii.
	void prewarmCircles(int maxRadius);

private:
	struct Snapped { int x0, x1, y0, y1, rad, rx, ry; };
	Snapped snap(Rect rc, float radius) const;
	SDL_FRect map(Rect rc) const { return {rc.x * sx, rc.y, rc.w * sx, rc.h}; }
	SDL_Texture* circleTexture(int radius);
	SDL_Texture* iconTexture(Icon kind, int size);

	int masks = 0;
	SDL_Window* win = nullptr;
	SDL_Renderer* r = nullptr;
	int W = 640, H = 480;
	float sx = 1.0f;
	std::unordered_map<int, SDL_Texture*> circles;
	std::unordered_map<int, SDL_Texture*> icons;
	SDL_Texture* bgTex = nullptr;
	SDL_Texture* cursorFill = nullptr;
	SDL_Texture* cursorEdge = nullptr;
	std::vector<SDL_Rect> clips;
};
