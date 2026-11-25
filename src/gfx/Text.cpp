#include "gfx/Text.h"
#include "gfx/TextEvict.h"
#include "gfx/Wrap.h"

#include <algorithm>
#include <cmath>

#include "core/Log.h"

extern "C" {
extern const unsigned char asset_font_medium[], asset_font_medium_end[];
extern const unsigned char asset_font_bold[], asset_font_bold_end[];
}

namespace {
constexpr int kMaxTextureW = 1024; // Wii GX limit
constexpr size_t kCacheSoftLimit = 400;
constexpr Uint32 kEvictAfterFrames = 180;

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// Byte index of the previous UTF-8 character boundary before pos.
size_t prevChar(std::string_view s, size_t pos) {
	if (pos == 0) return 0;
	pos--;
	while (pos > 0 && isContinuation((unsigned char)s[pos])) pos--;
	return pos;
}
} // namespace

bool Text::init(SDL_Renderer* renderer, float scaleX) {
	r = renderer;
	sx = scaleX;
	if (TTF_Init() != 0) { LOG("TTF_Init: %s", TTF_GetError()); return false; }
	return font(Font::Medium, 16) && font(Font::Bold, 16);
}

void Text::shutdown() {
	for (auto& [k, e] : cache) SDL_DestroyTexture(e.tex);
	cache.clear();
	cacheBytes_ = 0;
	for (auto& [k, f] : fonts) TTF_CloseFont(f);
	fonts.clear();
	TTF_Quit();
}

TTF_Font* Text::font(Font f, int size) {
	int key = (int)f * 1000 + size;
	auto it = fonts.find(key);
	if (it != fonts.end()) return it->second;

	const unsigned char* begin = f == Font::Bold ? asset_font_bold : asset_font_medium;
	const unsigned char* end = f == Font::Bold ? asset_font_bold_end : asset_font_medium_end;
	SDL_RWops* rw = SDL_RWFromConstMem(begin, (int)(end - begin));
	TTF_Font* font = TTF_OpenFontRW(rw, 1, size);
	if (!font) LOG("TTF_OpenFontRW(%d): %s", size, TTF_GetError());
	else TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
	fonts[key] = font;
	return font;
}

const Text::Entry* Text::get(Font f, int size, std::string_view s) {
	if (s.empty()) return nullptr;
	const int fs = fontKey(f, size);
	auto it = cache.find(KeyView{fs, s});
	if (it != cache.end()) {
		it->second.lastUsed = frame;
		return &it->second;
	}

	TTF_Font* fnt = font(f, size);
	if (!fnt) return nullptr;
	std::string str(s);
	SDL_Surface* surf = TTF_RenderUTF8_Blended(fnt, str.c_str(), SDL_Color{255, 255, 255, 255});
	if (!surf) return nullptr;
	SDL_Rect src{0, 0, std::min(surf->w, kMaxTextureW), surf->h};
	SDL_Surface* clipped = surf;
	if (surf->w > kMaxTextureW) {
		clipped = SDL_CreateRGBSurfaceWithFormat(0, src.w, src.h, 32, surf->format->format);
		SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(surf, &src, clipped, nullptr);
	}
	SDL_Texture* tex = SDL_CreateTextureFromSurface(r, clipped);
	if (clipped != surf) SDL_FreeSurface(clipped);
	SDL_FreeSurface(surf);
	if (!tex) return nullptr;
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

	cacheBytes_ += size_t(src.w) * size_t(src.h) * 4;
	auto [ins, ok] = cache.emplace(Key{fs, std::string(s)}, Entry{tex, src.w, src.h, frame});
	return &ins->second;
}

void Text::endFrame() {
	frame++;
	// Strings drawn this frame have lastUsed == frame - 1 now; they are never evicted here.
	auto drop = [&](auto it) {
		cacheBytes_ -= size_t(it->second.w) * size_t(it->second.h) * 4;
		SDL_DestroyTexture(it->second.tex);
		return cache.erase(it);
	};
	if (cache.size() >= kCacheSoftLimit) {
		for (auto it = cache.begin(); it != cache.end();) {
			if (frame - it->second.lastUsed > kEvictAfterFrames) it = drop(it);
			else ++it;
		}
	}
	if (cacheBytes_ <= kByteBudget) return;
	// Over the byte budget. When every entry was drawn last frame (a screen that really needs this much text) there is
	// nothing to free: look for that with one allocation-free pass and say nothing, instead of sorting and logging at 60 Hz.
	bool anyOld = false;
	for (const auto& [k, e] : cache) {
		if (textevict::evictable(e.lastUsed, frame)) { anyOld = true; break; }
	}
	if (!anyOld) return;
	// Least recently used first, down to 3/4 of the budget so this does not run every frame.
	const size_t before = cacheBytes_, count = cache.size();
	std::vector<textevict::Candidate> cand;
	std::vector<const Key*> keys;
	cand.reserve(cache.size());
	keys.reserve(cache.size());
	for (auto& [k, e] : cache) {
		cand.push_back({e.lastUsed, size_t(e.w) * size_t(e.h) * 4});
		keys.push_back(&k);
	}
	const std::vector<size_t> victims = textevict::choose(cand, cacheBytes_, kByteBudget, kByteBudget / 4 * 3, frame);
	for (size_t i : victims) drop(cache.find(KeyView{keys[i]->fs, keys[i]->s}));
	if (!victims.empty())
		LOG("Text cache over budget: %zu KB in %zu textures -> %zu KB in %zu", before / 1024, count, cacheBytes_ / 1024, cache.size());
}

int Text::width(Font f, int size, std::string_view s) {
	if (s.empty()) return 0;
	const int fs = fontKey(f, size);
	auto it = widths.find(KeyView{fs, s});
	if (it != widths.end()) return it->second;
	int w = 0, h = 0;
	if (TTF_Font* fnt = font(f, size)) {
		std::string str(s); // TTF_SizeUTF8 wants a terminated string; only a miss pays for the copy
		TTF_SizeUTF8(fnt, str.c_str(), &w, &h);
	}
	if (widths.size() > 4000) widths.clear();
	widths.emplace(Key{fs, std::string(s)}, w);
	return w;
}

int Text::lineHeight(Font f, int size) {
	TTF_Font* fnt = font(f, size);
	return fnt ? TTF_FontHeight(fnt) : size;
}

int Text::draw(Font f, int size, std::string_view s, float x, float y, Color c, Align a) {
	const Entry* e = get(f, size, s);
	if (!e) return 0;
	if (a == Align::Center) x -= e->w / 2.0f;
	else if (a == Align::Right) x -= e->w;
	SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
	SDL_SetTextureAlphaMod(e->tex, c.a);
	SDL_FRect d{std::round(x * sx), std::round(y), e->w * sx, (float)e->h};
	SDL_RenderCopyF(r, e->tex, nullptr, &d);
	return e->w;
}

void Text::drawIn(Font f, int size, std::string_view s, Rect rc, Color c, Align a) {
	std::string fit = ellipsize(f, size, s, (int)rc.w);
	float y = rc.cy() - lineHeight(f, size) / 2.0f;
	float x = a == Align::Left ? rc.x : a == Align::Center ? rc.cx() : rc.right();
	draw(f, size, fit, x, y, c, a);
}

std::string Text::ellipsize(Font f, int size, std::string_view s, int maxW) {
	if (width(f, size, s) <= maxW) return std::string(s);
	static constexpr std::string_view dots = "…";
	size_t lo = 0, hi = s.size();
	// Binary search on byte length, snapping to character boundaries.
	while (lo < hi) {
		size_t mid = (lo + hi + 1) / 2;
		while (mid < s.size() && isContinuation((unsigned char)s[mid])) mid++;
		std::string cand(s.substr(0, mid));
		cand += dots;
		if (width(f, size, cand) <= maxW) lo = mid;
		else hi = prevChar(s, mid);
	}
	std::string out(s.substr(0, lo));
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out + std::string(dots);
}

std::vector<std::string> Text::wrap(Font f, int size, std::string_view s, int maxW, int maxLines) {
	textwrap::Wrapper w([&](std::string_view t) { return width(f, size, t); }, s, maxW, maxLines > 0 ? (size_t)maxLines : 0);
	w.step();
	std::vector<std::string> lines = w.lines();
	if (w.truncated() && !lines.empty()) lines.back() = ellipsize(f, size, lines.back() + " …", maxW);
	return lines;
}
