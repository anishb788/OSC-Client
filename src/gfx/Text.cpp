#include "gfx/Text.h"

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

std::string Text::key(Font f, int size, std::string_view s) {
	std::string k;
	k.reserve(s.size() + 8);
	k += (char)('0' + (int)f);
	k += std::to_string(size);
	k += '|';
	k += s;
	return k;
}

const Text::Entry* Text::get(Font f, int size, std::string_view s) {
	if (s.empty()) return nullptr;
	std::string k = key(f, size, s);
	auto it = cache.find(k);
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

	auto [ins, ok] = cache.emplace(std::move(k), Entry{tex, src.w, src.h, frame});
	return &ins->second;
}

void Text::endFrame() {
	frame++;
	if (cache.size() < kCacheSoftLimit) return;
	for (auto it = cache.begin(); it != cache.end();) {
		if (frame - it->second.lastUsed > kEvictAfterFrames) {
			SDL_DestroyTexture(it->second.tex);
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

int Text::width(Font f, int size, std::string_view s) {
	if (s.empty()) return 0;
	std::string k = key(f, size, s);
	auto it = widths.find(k);
	if (it != widths.end()) return it->second;
	int w = 0, h = 0;
	if (TTF_Font* fnt = font(f, size)) TTF_SizeUTF8(fnt, k.c_str() + (k.find('|') + 1), &w, &h);
	if (widths.size() > 4000) widths.clear();
	widths.emplace(std::move(k), w);
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
	std::vector<std::string> lines;
	// Defensive: '\r' has no glyph (renders as a box), so drop it. Catalog text is already sanitized.
	std::string noCr;
	if (s.find('\r') != std::string_view::npos) {
		for (char c : s) if (c != '\r') noCr += c;
		s = noCr;
	}
	auto push = [&](std::string line) {
		lines.push_back(std::move(line));
		return maxLines > 0 && (int)lines.size() >= maxLines;
	};

	size_t pos = 0;
	while (pos <= s.size()) {
		size_t nl = s.find('\n', pos);
		std::string_view para = s.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
		pos = nl == std::string_view::npos ? s.size() + 1 : nl + 1;

		std::string line;
		size_t i = 0;
		if (para.empty() && push("")) break;
		while (i < para.size()) {
			size_t sp = para.find(' ', i);
			std::string_view word = para.substr(i, sp == std::string_view::npos ? std::string_view::npos : sp - i);
			i = sp == std::string_view::npos ? para.size() : sp + 1;

			std::string cand = line.empty() ? std::string(word) : line + " " + std::string(word);
			if (width(f, size, cand) <= maxW) { line = std::move(cand); continue; }

			if (!line.empty()) {
				if (push(std::move(line))) goto done;
				line.clear();
			}
			// A single word wider than the line: hard-break it.
			std::string w(word);
			while (width(f, size, w) > maxW && w.size() > 1) {
				size_t cut = w.size();
				while (cut > 1 && width(f, size, std::string_view(w).substr(0, cut)) > maxW) cut = prevChar(w, cut);
				if (push(w.substr(0, cut))) goto done;
				w.erase(0, cut);
			}
			line = std::move(w);
		}
		if (!line.empty() && push(std::move(line))) break;
	}
done:
	if (maxLines > 0 && (int)lines.size() == maxLines && !lines.empty()) {
		// Mark truncation if there was more text.
		size_t used = 0;
		for (auto& l : lines) used += l.size();
		if (used + lines.size() < s.size()) lines.back() = ellipsize(f, size, lines.back() + " …", maxW);
	}
	return lines;
}
