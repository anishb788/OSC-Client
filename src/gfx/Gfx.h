#pragma once
#include <functional>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

enum class Icon { ChevronLeft, ChevronRight, ChevronUp, ChevronDown, Search, Download, Warning, Note, Close, Refresh, Gear };

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

	void fillRect(Rect rc, Color c);
	void gradientV(Rect rc, Color top, Color bottom);
	void fillRoundRect(Rect rc, float radius, Color c);
	// Opaque rounded rect with a border of width bw.
	void roundRectBorder(Rect rc, float radius, float bw, Color border, Color fill);
	void softShadow(Rect rc, float radius, float strength = 1.0f);
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

private:
	SDL_FRect map(Rect rc) const { return {rc.x * sx, rc.y, rc.w * sx, rc.h}; }
	SDL_Texture* circleTexture(int radius);
	SDL_Texture* iconTexture(Icon kind, int size);

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
