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

	// Text textures are an LRU bounded by kByteBudget (estimated from w*h*4) as well as by age; see endFrame().
	static constexpr size_t kByteBudget = 8u << 20;
	size_t cacheBytes() const { return cacheBytes_; }
	size_t cacheCount() const { return cache.size(); }
	size_t widthCount() const { return widths.size(); }

	std::string ellipsize(Font f, int size, std::string_view s, int maxW);
	std::vector<std::string> wrap(Font f, int size, std::string_view s, int maxW, int maxLines = 0);

private:
	struct Entry { SDL_Texture* tex; int w, h; Uint32 lastUsed; };
	// Cache keys: font+size and the text. Lookups use the view type (heterogeneous lookup), so a cache hit, which is
	// nearly every call, allocates nothing; only a miss builds a std::string.
	struct Key { int fs; std::string s; };
	struct KeyView { int fs; std::string_view s; };
	struct KeyHash {
		using is_transparent = void;
		size_t operator()(const KeyView& k) const { return std::hash<std::string_view>{}(k.s) ^ (size_t(k.fs) * 0x9E3779B1u); }
		size_t operator()(const Key& k) const { return (*this)(KeyView{k.fs, k.s}); }
	};
	struct KeyEq {
		using is_transparent = void;
		template <class A, class B> bool operator()(const A& a, const B& b) const { return a.fs == b.fs && std::string_view(a.s) == std::string_view(b.s); }
	};
	static int fontKey(Font f, int size) { return (int)f * 1000 + size; }
	TTF_Font* font(Font f, int size);
	const Entry* get(Font f, int size, std::string_view s);

	SDL_Renderer* r = nullptr;
	float sx = 1.0f;
	std::unordered_map<int, TTF_Font*> fonts;
	std::unordered_map<Key, Entry, KeyHash, KeyEq> cache;
	std::unordered_map<Key, int, KeyHash, KeyEq> widths; // measuring shapes text, so memoize it
	size_t cacheBytes_ = 0;
	Uint32 frame = 0;
};
