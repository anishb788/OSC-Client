#include "App.h"
#include "audio/Audio.h"
#include "gfx/Theme.h"
#include "platform/Platform.h"
#include "ui/Widgets.h"

namespace {
class HomeScreen : public Screen {
public:
	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();
		const float m = theme::margin;
		float top = theme::topBarH + 18, bottom = H - theme::bottomBarH - 16;

		drawTopBar(ui, "OpenShop", "Open Shop Channel client");

		// Left column: featured app + quick actions.
		float lw = std::round(W * 0.40f);
		Rect feat{m, top, lw, 186};
		// Future in-session refresh: `f` is only used within this frame (and handed to Detail, see there), so a
		// replaced catalog would be picked up automatically here; only the pushed screens keep references.
		if (const AppEntry* f = app.catalog.featured()) {
			Id id = makeId("home:featured");
			if (ui.item(id, feat)) app.push(makeDetailScreen(*f));
			drawCard(ui, feat, ui.hover(id), 16);
			drawChip(ui, feat.x + 16, feat.y + 14, "FEATURED APP", theme::accentDark);
			drawAppIcon(ui, *f, {feat.x + 16, feat.y + 46, 128, 48});
			float tx = feat.x + 16;
			ui.t.drawIn(Font::Bold, 20, f->name, {tx, feat.y + 104, feat.w - 32, 26}, theme::text);
			ui.t.drawIn(Font::Medium, 14, "by " + f->author, {tx, feat.y + 128, feat.w - 32, 20}, theme::textMuted);
			ui.t.drawIn(Font::Medium, 15, f->shortDesc, {tx, feat.y + 150, feat.w - 32, 22}, theme::text);
			ui.setDefaultFocus(id);
		}

		static const Icon search = Icon::Search;
		float y = feat.bottom() + 16;
		if (pillButton(ui, makeId("home:search"), {m, y, lw, 52}, "Search apps", Pill::Primary, &search, 19))
			app.push(makeSearchScreen());
		y += 52 + 12;
		float half = (lw - 12) / 2;
		float bh = std::min(48.0f, bottom - y);
		if (pillButton(ui, makeId("home:popular"), {m, y, half, bh}, "Popular", Pill::Normal, nullptr, 17))
			app.push(makeListScreen("Popular", app.catalog.inCategory(""), kAllSorts, SortMode::Popular));
		if (pillButton(ui, makeId("home:newest"), {m + half + 12, y, half, bh}, "Newest", Pill::Normal, nullptr, 17))
			app.push(makeListScreen("Newest", app.catalog.inCategory(""), kAllSorts, SortMode::Newest));

		// Right: category tiles (2 x 3).
		float gx = m + lw + 18, gw = W - gx - m;
		float gap = 12, tw = (gw - gap) / 2, th = (bottom - top - 2 * gap) / 3;
		struct Tile { std::string name, label; };
		std::vector<Tile> tiles;
		for (auto& c : app.catalog.categories) tiles.push_back({c.name, c.display});
		tiles.push_back({"", "All apps"});
		for (size_t i = 0; i < tiles.size() && i < 6; i++) {
			Rect r{gx + (i % 2) * (tw + gap), top + (i / 2) * (th + gap), tw, th};
			Id id = makeId("home:tile:" + tiles[i].name, (uint32_t)i);
			if (ui.item(id, r)) {
				app.push(makeListScreen(tiles[i].label, app.catalog.inCategory(tiles[i].name), kAllSorts,
				                        SortMode::Popular));
			}
			drawTile(app, ui, r, ui.hover(id), tiles[i].name, tiles[i].label);
		}

		drawBottomBar(ui);
		float by = H - theme::bottomBarH + 12;
		ui.silenceNextActivation(); // quitPrompt() plays the HOME chime instead of the click
		bool exitClicked = pillButton(ui, makeId("home:exit"), {m, by, 120, 40}, platform::exitLabel(),
		                              Pill::Normal, nullptr, 17);
		if (exitClicked || ui.pressed(BTN_BACK)) app.quitPrompt();
		ui.t.drawIn(Font::Medium, 14, "Catalog provided by the Open Shop Channel",
		            {m + 136, by, W - 2 * m - 136 - 150, 40}, theme::textMuted, Align::Center);
		if (audio::hasMusic()) {
			static const Icon note = Icon::Note;
			bool on = audio::musicEnabled();
			if (pillButton(ui, makeId("home:music"), {W - m - 134, by, 134, 40}, on ? "Music on" : "Music off",
			               on ? Pill::Selected : Pill::Normal, &note, 15))
				audio::toggleMusic();
		}
	}

private:
	void drawTile(App& app, Ui& ui, Rect r, float hov, const std::string& cat, const std::string& label) {
		drawCard(ui, r, hov, 16);
		Rect g = r.inset(-2.5f * hov);
		Color c = categoryColor(cat);
		int n = app.catalog.count(cat);
		std::string count = std::to_string(n) + (n == 1 ? " app" : " apps");
		// Wide tiles (16:9): badge left of the text. Narrow tiles (4:3): badge above it.
		bool wide = g.w >= 190;
		float cr = wide ? std::min(22.0f, g.h * 0.24f) : std::min(17.0f, g.h * 0.19f);
		float cx = g.x + (wide ? 18 : 14) + cr, cy = wide ? g.cy() : g.y + 12 + cr;
		ui.g.circle(cx, cy, cr, c);
		ui.g.fillRoundRect({cx - cr * 0.62f, cy - cr * 0.82f, cr * 1.24f, cr * 0.6f}, cr * 0.3f, Color{255, 255, 255, 60});
		ui.t.drawIn(Font::Bold, (int)(cr * 1.05f), label.substr(0, 1), {cx - cr, cy - cr, cr * 2, cr * 2 - 1}, theme::white,
		            Align::Center);
		if (wide) {
			float tx = cx + cr + 12;
			ui.t.drawIn(Font::Bold, 18, label, {tx, cy - 22, g.right() - tx - 10, 24}, theme::text);
			ui.t.drawIn(Font::Medium, 14, count, {tx, cy + 1, g.right() - tx - 10, 20}, theme::textMuted);
		} else {
			float tx = g.x + 14, ty = cy + cr + 4;
			ui.t.drawIn(Font::Bold, 17, label, {tx, ty, g.w - 24, 22}, theme::text);
			ui.t.drawIn(Font::Medium, 13, count, {tx, ty + 20, g.w - 24, 18}, theme::textMuted);
		}
	}
};
} // namespace

std::unique_ptr<Screen> makeHomeScreen() { return std::make_unique<HomeScreen>(); }
