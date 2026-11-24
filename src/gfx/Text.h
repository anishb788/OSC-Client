#pragma once
#include <SDL_ttf.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

enum class Font { Medium, Bold };
enum class Align { Left, Center, Right };

// Text rendering with a texture cache. Strings are rendered once in white and
// tinted at draw time, so the same string in different colors shares a texture.
class Text {
public:
	bool init(SDL_Renderer* r, float scaleX = 1.0f);
	void shutdown();
	void endFrame(); // evicts textures not used recently

	int width(Font f, int size, std::string_view s);
	int lineHeight(Font f, int size);

	// (x, y) is the top-left of the line box (or top-center / top-right per align).
	// Returns the drawn width.
	int draw(Font f, int size, std::string_view s, float x, float y, Color c, Align a = Align::Left);
	// Draws s vertically centered in rc, ellipsized to fit.
	void drawIn(Font f, int size, std::string_view s, Rect rc, Color c, Align a = Align::Left);

	std::string ellipsize(Font f, int size, std::string_view s, int maxW);
	std::vector<std::string> wrap(Font f, int size, std::string_view s, int maxW, int maxLines = 0);

private:
	struct Entry { SDL_Texture* tex; int w, h; Uint32 lastUsed; };
	TTF_Font* font(Font f, int size);
	const Entry* get(Font f, int size, std::string_view s);
	static std::string key(Font f, int size, std::string_view s);

	SDL_Renderer* r = nullptr;
	float sx = 1.0f;
	std::unordered_map<int, TTF_Font*> fonts;
	std::unordered_map<std::string, Entry> cache;
	std::unordered_map<std::string, int> widths; // measuring shapes text, so memoize it
	Uint32 frame = 0;
};
