#include <cctype>
#include <cmath>

#include "App.h"
#include "audio/Audio.h"
#include "core/Log.h"
#include "gfx/Theme.h"
#include "ui/Widgets.h"

namespace {
const char* const kRows[] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.+"};
constexpr size_t kMaxQuery = 40;

// Future in-session refresh: `results` holds `const AppEntry*` into App::catalog, which is replaced
// only while the LoadingScreen is the only screen, so a Refresh destroys this screen first (see App::catalog). A
// refresh that kept the user's place would re-run the query (`app.catalog.search(query)`) after the swap instead
// of keeping these pointers, or hold a shared_ptr<const Catalog> snapshot.
// Search with an on-screen keyboard (pointer or D-pad); USB / PC keyboards type directly.
class SearchScreen : public Screen {
public:
	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();
		const float m = 48;

		// Physical keyboard input. Enter submits instead of clicking the focused key, except on a focused top-bar
		// capsule, where Enter presses the capsule like A does.
		bool enterSubmits = ui.onTop() && ui.in.enterKey && !anyTopBarFocused(ui);
		if (enterSubmits) ui.consumeConfirm();
		if (ui.onTop()) {
			appendTyped(ui.in.text);
			for (int i = 0; i < ui.in.backspaces; i++) erase();
		}

		app.topBar(ui, "Search", "Find apps by name, author or description");

		// Query field.
		Rect field{m, theme::topBarH + 16, W - 2 * m, 48};
		ui.g.softShadow(field, 24, 0.8f);
		ui.g.roundRectBorder(field, 24, 2.5f, theme::accent, theme::card);
		ui.g.icon(Icon::Search, {field.x + 14, field.cy() - 12, 24, 24}, theme::accent);
		float tx = field.x + 48;
		if (query.empty()) {
			ui.t.drawIn(Font::Medium, 19, "Type to search…", {tx, field.y, field.w - 160, field.h}, theme::textMuted);
		} else {
			std::string shown = query;
			int maxW = (int)field.w - 200;
			// Trim from the left, one whole character at a time.
			while (ui.t.width(Font::Bold, 20, shown) > maxW && shown.size() > 1) {
				size_t cut = 1;
				while (cut < shown.size() && ((unsigned char)shown[cut] & 0xC0) == 0x80) cut++;
				shown.erase(0, cut);
			}
			int w = ui.t.draw(Font::Bold, 20, shown, tx, field.cy() - ui.t.lineHeight(Font::Bold, 20) / 2.0f, theme::text);
			if (std::fmod(ui.time, 1.0) < 0.55) ui.g.fillRect({tx + w + 2, field.y + 12, 2, field.h - 24}, theme::accent);
		}
		std::string count = query.empty() ? "" : formatCount((uint32_t)results.size()) + (results.size() == 1 ? " match" : " matches");
		ui.t.drawIn(Font::Medium, 15, count, {field.right() - 150, field.y, 132, field.h}, theme::textMuted, Align::Right);

		// Keyboard.
		float ky = field.bottom() + 14, gap = 6;
		float kw = std::floor((W - 2 * m - 9 * gap) / 10), kh = 40;
		float kx = (W - (10 * kw + 9 * gap)) / 2;
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 10; col++) {
				char c = kRows[row][col];
				// Ids use the lowercase char so focus stays put when Shift flips the labels.
				std::string label(1, shift ? (char)std::toupper((unsigned char)c) : c);
				Rect r{kx + col * (kw + gap), ky + row * (kh + gap), kw, kh};
				Id id = makeId("key", (uint32_t)c);
				if (key(ui, id, r, label)) append(label);
				if (c == 'q') ui.setDefaultFocus(id);
			}
		}
		float by = ky + 4 * (kh + gap) + 4;
		// Bottom row: five keys, each two letter-keys wide.
		float wide = 2 * kw + gap;
		float bx = kx;
		if (key(ui, makeId("key:shift"), {bx, by, wide, kh}, "Shift", shift)) shift = !shift;
		bx += wide + gap;
		if (key(ui, makeId("key:space"), {bx, by, wide, kh}, "Space")) append(" ");
		bx += wide + gap;
		if (key(ui, makeId("key:del"), {bx, by, wide, kh}, "Delete")) erase();
		bx += wide + gap;
		if (key(ui, makeId("key:clear"), {bx, by, wide, kh}, "Clear")) { query.clear(); update(app); }
		bx += wide + gap;
		bool submit = pillButton(ui, makeId("key:go"), {bx, by, wide, kh}, "Search",
		                         {.style = Pill::Primary, .icon = Icon::Search, .fontSize = 16,
		                          .radius = 10}); // radius 10 like the other keys
		if (enterSubmits) submit = true;

		if (dirty) update(app);
		if (submit) {
			if (query.empty() || results.empty()) {
				audio::play(audio::Sfx::Error);
			} else {
				openResults(app);
			}
		}

		drawBottomBar(ui);
		if (backButton(ui)) app.pop();
		ui.t.drawIn(Font::Medium, 14, "+ searches", {W - theme::margin - 300, H - theme::bottomBarH + 12, 300, 40},
		            theme::textMuted, Align::Right);
		if (ui.pressed(BTN_PLUS) && !query.empty() && !results.empty()) openResults(app);
	}

private:
	void openResults(App& app) {
		app.push(makeListScreen("“" + query + "”", results, kSearchSorts, SortMode::Relevance));
	}

	// A keyboard key: a glossy rounded-rectangle button (pillButton, radius 10); `on` draws it latched (Shift).
	bool key(Ui& ui, Id id, Rect r, const std::string& label, bool on = false) {
		return pillButton(ui, id, r, label,
		                  {.style = on ? Pill::Selected : Pill::Normal, .fontSize = label.size() == 1 ? 20 : 15,
		                   .radius = 10});
	}

	// Typed text arrives as UTF-8. It is appended one whole character at a time (never half of a multibyte sequence, so
	// the 40-byte cap always falls on a character boundary), and control characters, DEL and damaged sequences are dropped.
	void appendTyped(const std::string& text) {
		for (size_t i = 0; i < text.size();) {
			const unsigned char c = (unsigned char)text[i];
			size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
			bool ok = n > 0 && i + n <= text.size();
			for (size_t k = 1; ok && k < n; k++) ok = ((unsigned char)text[i + k] & 0xC0) == 0x80;
			if (!ok) { i++; continue; }
			// Stop at the first character that doesn't fit: one Error sound per typing event, and a later, shorter
			// character never slips in after a longer one was refused.
			if (c >= 0x20 && c != 0x7F && !append(text.substr(i, n))) return;
			i += n;
		}
	}

	// Returns false (after the Error sound) when the query is full.
	bool append(const std::string& s) {
		if (query.size() + s.size() > kMaxQuery) { audio::play(audio::Sfx::Error); return false; }
		if (s == " " && (query.empty() || query.back() == ' ')) return true;
		query += s;
		dirty = true;
		return true;
	}

	void erase() {
		if (query.empty()) return;
		// Remove one UTF-8 character.
		size_t i = query.size() - 1;
		while (i > 0 && ((unsigned char)query[i] & 0xC0) == 0x80) i--;
		query.erase(i);
		dirty = true;
	}

	void update(App& app) {
		Uint64 t0 = SDL_GetPerformanceCounter();
		results = app.catalog.search(query);
		double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
		if (!query.empty()) LOG("search \"%s\": %d results in %.3f ms (%d apps)", query.c_str(), (int)results.size(), ms,
		                      (int)app.catalog.apps.size());
		dirty = false;
	}

	std::string query;
	std::vector<const AppEntry*> results;
	bool dirty = false;
	bool shift = false; // caps toggle for the on-screen letter keys
};
} // namespace

std::unique_ptr<Screen> makeSearchScreen() { return std::make_unique<SearchScreen>(); }
