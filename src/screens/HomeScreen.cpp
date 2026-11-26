#include "App.h"
#include "audio/Audio.h"
#include "gfx/Theme.h"
#include "platform/Platform.h"
#include "ui/HomeLayout.h"
#include "ui/TopBarLayout.h"
#include "ui/Widgets.h"

namespace {
class HomeScreen : public Screen {
public:
	explicit HomeScreen(std::optional<RefreshNotice> notice) : notice_(std::move(notice)) {}

	void draw(App& app, Ui& ui) override {
		// A failed Refresh lands here: the dialog can't be shown before the reset (the swap drops dialogs), so
		// Home shows it once the fade-in has finished. It waits one more input-enabled frame: on the first one the
		// modal depth was still 0, so a press in that same frame could otherwise reach Home under the new dialog.
		if (notice_ && ui.inputEnabled && !app.dialogOpen() && settled_++ >= 1) {
			Dialog d;
			d.title = "Couldn't refresh";
			d.message = notice_->reason + "\nShowing the catalog from " + notice_->fetchedAt + ".";
			d.primaryIndex = 0;
			app.showDialog(std::move(d));
			notice_.reset();
		}
		float W = (float)ui.g.width(), H = (float)ui.g.height();
		const float m = theme::margin;
		const homelayout::Layout L = homelayout::layout(W, H, m, theme::bottomBarH);
		auto R = [](const homelayout::Box& b) { return Rect{b.x, b.y, b.w, b.h}; };

		// Offline (a cache read because the network failed, or the offline option): the chip takes the subtitle's place
		// in the top bar. That spot is clear of the capsules (the chip is kept left of where all three would sit), of
		// the left column and of the bottom bar's hint and credit.
		const bool offline = app.catalog.offline;
		app.topBar(ui, "OpenShop", offline ? std::string_view() : std::string_view("Open Shop Channel client"));
		if (offline) drawOfflineChip(ui, W, app.catalog.fetchedAt);

		// Left column: featured app + quick actions.
		Rect feat = R(L.featured);
		// Future in-session refresh: the catalog is replaced only while the LoadingScreen is the only screen, so
		// this Home (like every holder of an AppEntry pointer) is destroyed first. `f` is only used within this
		// frame (and handed to Detail, see there), so a refresh that kept Home alive would pick up the new catalog
		// automatically; only the pushed screens keep references.
		if (const AppEntry* f = app.catalog.featured()) {
			Id id = makeId("home:featured");
			if (ui.item(id, feat)) app.push(makeDetailScreen(*f));
			drawCard(ui, feat, ui.hover(id), 16);
			drawFeatured(ui, feat, L.wideColumn, *f);
			ui.setDefaultFocus(id);
		}

		// Without a featured app, Search is the D-pad's starting point (never a top-bar capsule).
		if (!app.catalog.featured()) ui.setDefaultFocus(makeId("home:search"));
		if (pillButton(ui, makeId("home:search"), R(L.search), "Search apps",
		               {.style = Pill::Primary, .icon = Icon::Search, .fontSize = 19}))
			app.push(makeSearchScreen());
		if (pillButton(ui, makeId("home:popular"), R(L.popular), "Popular", {.fontSize = 17}))
			app.push(makeListScreen("Popular", app.catalog.inCategory(""), kAllSorts, SortMode::Popular));
		if (pillButton(ui, makeId("home:newest"), R(L.newest), "Newest", {.fontSize = 17}))
			app.push(makeListScreen("Newest", app.catalog.inCategory(""), kAllSorts, SortMode::Newest));
		// M3 passes the number of apps with updates; "N updates" shows only above 0 (M2 doesn't know, so no badge).
		const int updates = 0;
		std::string updatesText = updates > 0 ? std::to_string(updates) + (updates == 1 ? " update" : " updates") : "";
		if (pillButton(ui, makeId("home:myapps"), R(L.myApps), "My apps",
		               {.icon = Icon::Package, .fontSize = 17, .iconColor = theme::accentDark, .leftAlign = true,
		                .padLeft = 18, .badge = updatesText}))
			app.openMyApps();

		// Right: category tiles (2 x 3).
		struct Tile { std::string name, label; };
		std::vector<Tile> tiles;
		// The grid has six slots: the first five categories, then "All apps", whatever number the API returns (the rest
		// are reachable through All apps).
		for (size_t i = 0; i < app.catalog.categories.size() && i < 5; i++)
			tiles.push_back({app.catalog.categories[i].name, app.catalog.categories[i].display});
		tiles.push_back({"", "All apps"});
		for (size_t i = 0; i < tiles.size() && i < 6; i++) {
			Rect r{L.grid.x + (i % 2) * (L.tileW + homelayout::kTileGap),
			       L.grid.y + (i / 2) * (L.tileH + homelayout::kTileGap), L.tileW, L.tileH};
			Id id = makeId("home:tile:" + tiles[i].name, (uint32_t)i);
			if (ui.item(id, r)) {
				app.push(makeListScreen(tiles[i].label, app.catalog.inCategory(tiles[i].name), kAllSorts,
				                        SortMode::Popular));
			}
			drawTile(app, ui, r, ui.hover(id), L.wideTiles, tiles[i].name, tiles[i].label);
		}

#ifdef OPENSHOP_DEBUGTOOLS
		if (ui.pressed(BTN_MINUS)) app.push(makeNetTestScreen()); // debug builds: back to the network test bench
#endif
		drawBottomBar(ui);
		float by = H - theme::bottomBarH + 12;
		ui.silenceNextActivation(); // quitPrompt() plays the HOME chime instead of the click
		bool exitClicked = pillButton(ui, makeId("home:exit"), {m, by, 120, 40}, platform::exitLabel(),
		                              {.fontSize = 17});
		if (exitClicked || ui.pressed(BTN_BACK)) app.quitPrompt();

		// Bottom bar text: the credit, centred between Exit and the margin (or the music hint), and the hint
		// itself, right-aligned, only when background music exists.
		const char* hint = audio::hasMusic() ? platform::musicToggleHint() : "";
		float hintW = hint[0] ? (float)ui.t.width(Font::Medium, 14, hint) : 0;
		float left = m + 136, right = hint[0] ? W - m - hintW - 16 : W - m - 136;
		// Longest credit that fits; the hint (4:3 is the tight case) never gets pushed out.
		std::string credit = "Catalog provided by the Open Shop Channel / OSC Team";
		if (ui.t.width(Font::Medium, 14, credit) > right - left) credit = "Catalog provided by the Open Shop Channel";
		if (ui.t.width(Font::Medium, 14, credit) > right - left) credit = "Catalog: Open Shop Channel";
		ui.t.drawIn(Font::Medium, 14, credit, {left, by, right - left, 40}, theme::textMuted, Align::Center);
		if (hint[0]) ui.t.drawIn(Font::Medium, 14, hint, {W - m - hintW - 2, by, hintW + 2, 40}, theme::textMuted, Align::Right);
	}

private:
	static void drawOfflineChip(Ui& ui, float W, int64_t fetchedAt) {
		const float x = theme::margin + ui.t.width(Font::Bold, 24, "OpenShop") + 16;
		// The room left of the top-bar capsules, counted for all three (M3 and M4 add Downloads and Settings).
		const float right = W - theme::margin - 3 * topbar::kCapW - 2 * topbar::kGap - topbar::kTitleGap;
		// The same local date and time as the "Couldn't refresh" message (fetchedAt is a moment, not a calendar day, so
		// formatDate's UTC day would be wrong near midnight).
		std::string label = fetchedAt > 0 ? "Offline: catalog from " + formatDateTime(fetchedAt) : "Offline: saved catalog";
		if (x + chipWidth(ui, label, /*hasIcon=*/true) > right) label = "Offline";
		drawChip(ui, x, (theme::topBarH - 3) / 2 - 11, label, theme::warn, Icon::Warning);
	}

	std::optional<RefreshNotice> notice_;
	int settled_ = 0; // input-enabled frames seen while a notice waits

	// Featured card: chip, then icon + name / author side by side, then the short description on one line. The
	// narrow column (4:3, primary) uses a 96x36 icon and a 17 px name; the wide one (16:9) 128x48 and 19 px.
	void drawFeatured(Ui& ui, Rect card, bool wide, const AppEntry& f) {
		const float padX = wide ? 16 : 14, padY = 14;
		const float iw = wide ? 128 : 96, ih = wide ? 48 : 36, gapX = wide ? 14 : 12;
		const float chipH = 22, descH = 20;
		float free = card.h - 2 * padY - chipH - ih - descH;
		float gap = std::max(4.0f, free / 2);
		float x = card.x + padX, innerW = card.w - 2 * padX;
		drawChip(ui, x, card.y + padY, "FEATURED APP", theme::accentDark);
		float ry = card.y + padY + chipH + gap;
		drawAppIcon(ui, f, {x, ry, iw, ih});
		float tx = x + iw + gapX, tw = innerW - iw - gapX;
		int nameSize = wide ? 19 : 17;
		ui.t.drawIn(Font::Bold, nameSize, f.name, {tx, ry + ih / 2 - 24, tw, 24}, theme::text);
		ui.t.drawIn(Font::Medium, wide ? 14 : 13, "by " + f.author, {tx, ry + ih / 2, tw, 20}, theme::textMuted);
		ui.t.drawIn(Font::Medium, 14, f.shortDesc, {x, ry + ih + gap, innerW, descH}, theme::text);
	}

	// Flat blue pictogram (with a faint drop shadow) above the name and count; on wide tiles (16:9) to their left.
	void drawTile(App& app, Ui& ui, Rect r, float hov, bool wide, const std::string& cat, const std::string& label) {
		drawCard(ui, r, hov, 16);
		Rect g = r.inset(-2.5f * hov);
		int n = app.catalog.count(cat);
		std::string count = std::to_string(n) + (n == 1 ? " app" : " apps");
		Icon ic = categoryIcon(cat);
		float isz = ic == Icon::Grid ? 32.0f : (ic == Icon::Gamepad || ic == Icon::Sparkle) ? 38.0f : 36.0f;
		auto drawIcon = [&](Rect box) {
			Rect ir{box.cx() - isz / 2, box.cy() - isz / 2, isz, isz};
			ui.g.icon(ic, ir.offset(0, 1), Color{0, 60, 100, 46});
			ui.g.icon(ic, ir, theme::iconBlue);
		};
		// `wide` comes from the layout (homelayout::Layout::wideTiles), not from g: the hover growth must never flip it.
		if (wide) {
			Rect box{g.x + 16, g.cy() - 22, 44, 44};
			drawIcon(box);
			float tx = box.right() + 12;
			ui.t.drawIn(Font::Bold, 18, label, {tx, g.cy() - 22, g.right() - tx - 10, 24}, theme::text);
			ui.t.drawIn(Font::Medium, 14, count, {tx, g.cy() + 1, g.right() - tx - 10, 20}, theme::textMuted);
		} else {
			// Stacked: icon, name, count, centred vertically as a group, left-aligned at 14 px.
			const float icoH = 38, nameH = 22, countH = 18, gap = 2;
			float top = g.cy() - (icoH + gap + nameH + countH) / 2;
			drawIcon({g.x + 14, top, 38, icoH});
			float tx = g.x + 14, ty = top + icoH + gap;
			ui.t.drawIn(Font::Bold, 17, label, {tx, ty, g.w - 24, nameH}, theme::text);
			ui.t.drawIn(Font::Medium, 13, count, {tx, ty + nameH, g.w - 24, countH}, theme::textMuted);
		}
	}
};
} // namespace

std::unique_ptr<Screen> makeHomeScreen(std::optional<RefreshNotice> notice) {
	return std::make_unique<HomeScreen>(std::move(notice));
}
