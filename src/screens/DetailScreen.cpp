#include <algorithm>

#include "App.h"
#include "audio/Audio.h"
#include "gfx/Theme.h"
#include "ui/Widgets.h"

namespace {
// Future in-session refresh: this screen holds a `const AppEntry&` into App::catalog, valid because the catalog
// is installed once and never replaced (see App::catalog). For a refreshable catalog, store the slug instead,
// re-resolve it with `app.catalog.find(slug)` at the top of every draw() and pop() if it is gone (or hold a
// shared_ptr<const Catalog> snapshot taken when the screen was created).
class DetailScreen : public Screen {
public:
	explicit DetailScreen(const AppEntry& a) : app_(a) {}

	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();
		const float m = theme::margin;
		const AppEntry& a = app_;
		float top = theme::topBarH + 16, bottom = H - theme::bottomBarH - 14;

		drawTopBar(ui, a.name, app.catalog.categoryDisplay(a.category));

		// Left: icon + download.
		float lw = std::round(W * 0.34f);
		Rect iconCard{m, top, lw, 112};
		drawCard(ui, iconCard, 0, 16);
		float iw = std::min(lw - 28, 224.0f);
		drawAppIcon(ui, a, {iconCard.cx() - iw / 2, iconCard.cy() - iw * 48 / 256, iw, iw * 48 / 128});

		static const Icon dl = Icon::Download;
		Id dlId = makeId("detail:download");
		if (pillButton(ui, dlId, {m, iconCard.bottom() + 16, lw, 54}, "Download", Pill::Primary, &dl, 20))
			startDownload(app);
		ui.setDefaultFocus(dlId);
		ui.t.drawIn(Font::Medium, 14, formatSize(a.archiveSize) + " download",
		            {m, iconCard.bottom() + 76, lw, 20}, theme::textMuted, Align::Center);

		// Flags.
		float fy = iconCard.bottom() + 106;
		static const Icon warn = Icon::Warning;
		if (a.hasFlag("WRITES_TO_NAND")) { drawChip(ui, m, fy, "Modifies system memory", theme::warn, &warn); fy += 30; }
		if (a.hasFlag("DEPRECATED")) { drawChip(ui, m, fy, "Deprecated", theme::muted); fy += 30; }
		if (a.hasFlag("AI_ASSISTED")) { drawChip(ui, m, fy, "AI-assisted", theme::accentDark); fy += 30; }

		// Right: info and description.
		float rx = m + lw + 22, rw = W - rx - m;
		ui.t.drawIn(Font::Bold, 24, a.name, {rx, top - 2, rw, 30}, theme::text);
		ui.t.drawIn(Font::Medium, 15, "by " + a.author, {rx, top + 28, rw, 20}, theme::textMuted);

		float iy = top + 58, colW = rw / 2;
		std::string version = a.version.empty() ? "—" : a.version;
		info(ui, rx, iy, colW, "Version", version);
		info(ui, rx + colW, iy, colW, "Released", formatDate(a.releaseDate));
		iy += 42;
		info(ui, rx, iy, colW, "Downloads", formatCount(a.downloads));
		info(ui, rx + colW, iy, colW, "Works on", platforms(a));
		iy += 42;
		info(ui, rx, iy, colW, "Controllers", peripherals(a));
		info(ui, rx + colW, iy, colW, "Size on SD", formatSize(a.uncompressedSize));
		iy += 46;

		// Description panel with scrolling.
		Rect box{rx, iy, rw, bottom - iy};
		ui.g.fillRoundRect(box.inset(-1), 13, theme::border.withAlpha(0.6f));
		ui.g.fillRoundRect(box, 12, theme::panel);
		const int fs = 15;
		float lh = (float)ui.t.lineHeight(Font::Medium, fs);
		if (wrappedW != (int)box.w) {
			wrappedW = (int)box.w;
			lines = ui.t.wrap(Font::Medium, fs, a.longDesc.empty() ? a.shortDesc : a.longDesc, wrappedW - 48);
		}
		int visible = std::max(1, (int)((box.h - 20) / lh));
		int maxScroll = std::max(0, (int)lines.size() - visible);
		bool canScroll = maxScroll > 0;
		if (canScroll) {
			float ax = box.right() - 18;
			if (scroll > 0 && arrowButton(ui, makeId("detail:up"), ax, box.y + 20, Icon::ChevronUp, 13)) scroll -= visible - 1;
			if (scroll < maxScroll && arrowButton(ui, makeId("detail:down"), ax, box.bottom() - 20, Icon::ChevronDown, 13))
				scroll += visible - 1;
			scroll -= ui.wheel() * 3;
			if (ui.pressed(BTN_PLUS)) scroll += visible - 1;
			if (ui.pressed(BTN_MINUS)) scroll -= visible - 1;
			scroll = std::clamp(scroll, 0, maxScroll);
		}
		ui.g.pushClip(box.inset(4));
		for (int i = 0; i < visible + 1 && scroll + i < (int)lines.size(); i++)
			ui.t.draw(Font::Medium, fs, lines[scroll + i], box.x + 16, box.y + 10 + i * lh, theme::text);
		ui.g.popClip();
		if (canScroll) {
			// Scroll position indicator.
			float trackY = box.y + 40, trackH = box.h - 80;
			float thumbH = std::max(16.0f, trackH * visible / lines.size());
			float thumbY = trackY + (trackH - thumbH) * scroll / maxScroll;
			ui.g.fillRoundRect({box.right() - 20, trackY, 4, trackH}, 2, theme::border.withAlpha(0.6f));
			ui.g.fillRoundRect({box.right() - 20, thumbY, 4, thumbH}, 2, theme::accent);
		}

		drawBottomBar(ui);
		if (backButton(ui)) app.pop();
		ui.t.drawIn(Font::Medium, 14, canScroll ? "Press + / - to scroll" : "",
		            {W - m - 260, H - theme::bottomBarH + 12, 260, 40}, theme::textMuted, Align::Right);
	}

private:
	void info(Ui& ui, float x, float y, float w, std::string_view label, std::string_view value) {
		ui.t.drawIn(Font::Medium, 13, label, {x, y, w - 10, 16}, theme::textMuted);
		ui.t.drawIn(Font::Bold, 16, value, {x, y + 16, w - 10, 22}, theme::text);
	}

	static std::string platforms(const AppEntry& a) {
		std::string s;
		for (auto& p : a.platforms) s += (s.empty() ? "" : ", ") + platformName(p);
		return s.empty() ? "Unknown" : s;
	}

	static std::string peripherals(const AppEntry& a) {
		// Collapse duplicates ("wii_remote" x4 means up to four remotes).
		std::string s;
		std::vector<std::string> seen;
		for (auto& p : a.peripherals) {
			if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
			seen.push_back(p);
			int n = (int)std::count(a.peripherals.begin(), a.peripherals.end(), p);
			s += (s.empty() ? "" : ", ") + (n > 1 ? std::to_string(n) + "× " : "") + peripheralName(p);
		}
		return s.empty() ? "None listed" : s;
	}

	void startDownload(App& app) {
		Dialog d;
		d.kind = Dialog::Kind::Progress;
		d.title = "Downloading " + app_.name;
		d.message = "UI preview: no files are downloaded or written yet.";
		d.doneTitle = "Preview complete";
		d.doneMessage = "Installing apps arrives in milestone 3. Nothing was written to your SD card.";
		app.showDialog(std::move(d));
	}

	const AppEntry& app_;
	std::vector<std::string> lines;
	int wrappedW = -1;
	int scroll = 0;
};
} // namespace

std::unique_ptr<Screen> makeDetailScreen(const AppEntry& app) { return std::make_unique<DetailScreen>(app); }
