#include "App.h"
#include "audio/Audio.h"
#include "gfx/Theme.h"
#include "ui/Widgets.h"

namespace {
const char* sortLabel(SortMode m) {
	switch (m) {
	case SortMode::Popular: return "Popular";
	case SortMode::Newest: return "Newest";
	case SortMode::Name: return "A–Z";
	case SortMode::Relevance: return "Best match";
	}
	return "";
}

// Future in-session refresh: `base` and `apps` hold `const AppEntry*` into App::catalog, which is replaced
// only while the LoadingScreen is the only screen, so a Refresh destroys this screen first (see App::catalog). A
// refresh that kept the user's place would store slugs (or the category and sort mode and rebuild with
// Catalog::inCategory / search) and resolve them again after a swap, or keep a shared_ptr<const Catalog> snapshot
// alive for the screen's lifetime.
// Paged list of apps, the way the shop pages through titles.
class ListScreen : public Screen {
public:
	ListScreen(std::string title, std::vector<const AppEntry*> apps, std::vector<SortMode> sorts, SortMode initial,
	           std::string empty)
		: title(std::move(title)), base(std::move(apps)), sorts(std::move(sorts)), sort(initial),
		  emptyMessage(std::move(empty)) {
		resort();
	}

	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();
		const float side = 52;
		int pages = std::max(1, ((int)apps.size() + kPerPage - 1) / kPerPage);
		page = std::clamp(page, 0, pages - 1);

		std::string sub = formatCount((uint32_t)apps.size()) + (apps.size() == 1 ? " app" : " apps");
		app.topBar(ui, title, sub);

		// Sort pills.
		float y = theme::topBarH + 14;
		float px = side;
		// An empty list has no row 0; start the D-pad on the first sort pill (never on a top-bar capsule).
		if (apps.empty() && !sorts.empty()) ui.setDefaultFocus(makeId("list:sort", 0u));
		for (size_t i = 0; i < sorts.size(); i++) {
			const char* label = sortLabel(sorts[i]);
			float w = (float)ui.t.width(Font::Bold, 15, label) + 40;
			if (pillButton(ui, makeId("list:sort", (uint32_t)i), {px, y, w, 32}, label,
			               sorts[i] == sort ? Pill::Selected : Pill::Normal, std::nullopt, 15) &&
			    sorts[i] != sort) {
				sort = sorts[i];
				resort();
				page = 0;
				slide = 0.0f;
			}
			px += w + 10;
		}

		// Rows.
		float listTop = y + 32 + 14, listBottom = H - theme::bottomBarH - 10;
		float rowGap = 8, rowH = std::floor((listBottom - listTop - rowGap * (kPerPage - 1)) / kPerPage);
		slide = approach(slide, 0.0f, 18.0f, ui.dt);
		float dx = slide * 60;

		int first = page * kPerPage;
		int shown = std::min(kPerPage, (int)apps.size() - first);
		if (apps.empty()) {
			ui.t.drawIn(Font::Medium, 18, emptyMessage, {side, listTop, W - 2 * side, rowH * 2}, theme::textMuted,
			            Align::Center);
		}
		ui.g.pushClip({0, listTop - 8, W, listBottom - listTop + 16});
		for (int i = 0; i < shown; i++) {
			const AppEntry& a = *apps[first + i];
			Rect r{side + dx, listTop + i * (rowH + rowGap), W - 2 * side, rowH};
			Id id = makeId("list:row", (uint32_t)i);
			if (ui.item(id, r)) app.push(makeDetailScreen(a));
			if (i == 0) ui.setDefaultFocus(id);
			drawRow(ui, r, ui.hover(id), a);
		}
		ui.g.popClip();

		// Paging: arrows, +/-, Left/Right past the edge, wheel, Down past the last row.
		float mid = (listTop + listBottom) / 2;
		int want = page;
		if (page > 0 && arrowButton(ui, makeId("list:prev"), side / 2 + 2, mid, Icon::ChevronLeft, 19)) want--;
		if (page < pages - 1 && arrowButton(ui, makeId("list:next"), W - side / 2 - 2, mid, Icon::ChevronRight, 19)) want++;
		if (ui.pressed(BTN_MINUS) || (ui.unhandledNav() & BTN_LEFT) || ui.wheel() > 0) want--;
		if (ui.pressed(BTN_PLUS) || (ui.unhandledNav() & BTN_RIGHT) || ui.wheel() < 0) want++;
		if ((ui.unhandledNav() & BTN_DOWN) && page < pages - 1) {
			want = page + 1;
			ui.setFocus(makeId("list:row", 0));
		}
		want = std::clamp(want, 0, pages - 1);
		if (want != page) {
			slide = want > page ? 1.0f : -1.0f;
			page = want;
			audio::play(audio::Sfx::Page);
		}

		drawBottomBar(ui);
		if (backButton(ui)) app.pop();
		if (pages > 1) drawPageDots(ui, W / 2, H - theme::bottomBarH / 2.0f, pages);
	}

private:
	static constexpr int kPerPage = 5;

	// Always sort a copy of the original (relevance) order, so SortMode::Relevance can return to it.
	void resort() {
		apps = base;
		Catalog::sort(apps, sort);
	}

	void drawRow(Ui& ui, Rect r, float hov, const AppEntry& a) {
		drawCard(ui, r, hov, 12);
		Rect g = r.inset(-2.5f * hov);
		float ih = std::min(48.0f, g.h - 12);
		Rect icon{g.x + 10, g.cy() - ih / 2, ih * 128 / 48, ih};
		drawAppIcon(ui, a, icon);

		float tx = icon.right() + 16;
		float rightW = 120;
		float textW = g.right() - tx - rightW - 12;
		ui.t.drawIn(Font::Bold, 18, a.name, {tx, g.y + 5, textW, 24}, theme::text);
		ui.t.drawIn(Font::Medium, 14, a.author + "  ·  " + a.shortDesc, {tx, g.y + 28, textW, 20}, theme::textMuted);

		float rx = g.right() - 14;
		ui.t.drawIn(Font::Bold, 15, formatSize(a.archiveSize), {rx - rightW, g.y + 6, rightW, 22}, theme::text, Align::Right);
		std::string sub = a.version.empty() ? std::string() : (a.version[0] == 'v' ? a.version : "v" + a.version);
		ui.t.drawIn(Font::Medium, 13, sub, {rx - rightW, g.y + 28, rightW, 20}, theme::textMuted, Align::Right);
		if (a.hasFlag("WRITES_TO_NAND")) {
			ui.g.icon(Icon::Warning, {rx - rightW - 4, g.y + 8, 16, 16}, theme::warn);
		}
	}

	void drawPageDots(Ui& ui, float cx, float cy, int pages) {
		// Up to 9 dots; beyond that show "page / pages".
		if (pages > 9) {
			std::string s = "Page " + std::to_string(page + 1) + " / " + std::to_string(pages);
			ui.t.draw(Font::Bold, 16, s, cx, cy - ui.t.lineHeight(Font::Bold, 16) / 2.0f, theme::textMuted, Align::Center);
			return;
		}
		float gap = 18, x0 = cx - (pages - 1) * gap / 2;
		for (int i = 0; i < pages; i++) {
			bool cur = i == page;
			ui.g.circle(x0 + i * gap, cy, cur ? 5.5f : 4.0f, cur ? theme::accent : theme::border);
		}
	}

	std::string title;
	std::vector<const AppEntry*> base; // order as given by the caller
	std::vector<const AppEntry*> apps; // `base` sorted by `sort`
	std::vector<SortMode> sorts;
	SortMode sort;
	std::string emptyMessage;
	int page = 0;
	float slide = 0;
};
} // namespace

std::unique_ptr<Screen> makeListScreen(std::string title, std::vector<const AppEntry*> apps,
                                       std::vector<SortMode> sorts, SortMode initial, std::string emptyMessage) {
	return std::make_unique<ListScreen>(std::move(title), std::move(apps), std::move(sorts), initial,
	                                    std::move(emptyMessage));
}
