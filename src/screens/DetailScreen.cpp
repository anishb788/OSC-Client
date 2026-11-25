#include <algorithm>
#include <memory>

#include "App.h"
#include "audio/Audio.h"
#include "core/Log.h"
#include "gfx/Theme.h"
#include "gfx/Wrap.h"
#include "ui/Widgets.h"

namespace {
// Future in-session refresh: this screen holds a `const AppEntry&` into App::catalog, valid because the catalog
// is replaced only while the LoadingScreen is the only screen, so a Refresh destroys this screen first (see
// App::catalog). A refresh that kept the user's place would store the slug instead, re-resolve it with
// `app.catalog.find(slug)` at the top of every draw() and pop() if it is gone (or hold a shared_ptr<const Catalog>
// snapshot taken when the screen was created).
class DetailScreen : public Screen {
public:
	explicit DetailScreen(const AppEntry& a) : app_(a) {}

	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();
		const float m = theme::margin;
		const AppEntry& a = app_;
		float top = theme::topBarH + 16, bottom = H - theme::bottomBarH - 14;

		app.topBar(ui, a.name, app.catalog.categoryDisplay(a.category));

		// Left: icon + download.
		float lw = std::round(W * 0.34f);
		Rect iconCard{m, top, lw, 112};
		drawCard(ui, iconCard, 0, 16);
		float iw = std::min(lw - 28, 224.0f);
		drawAppIcon(ui, a, {iconCard.cx() - iw / 2, iconCard.cy() - iw * 48 / 256, iw, iw * 48 / 128});

		Id dlId = makeId("detail:download");
		if (pillButton(ui, dlId, {m, iconCard.bottom() + 16, lw, 54}, "Download",
		               {.style = Pill::Primary, .icon = Icon::Download, .fontSize = 20}))
			startDownload(app);
		ui.setDefaultFocus(dlId);
		ui.t.drawIn(Font::Medium, 14, formatSize(a.archiveSize) + " download",
		            {m, iconCard.bottom() + 76, lw, 20}, theme::textMuted, Align::Center);

		// Flags.
		float fy = iconCard.bottom() + 106;
		if (a.hasFlag("WRITES_TO_NAND")) { drawChip(ui, m, fy, "Modifies system memory", theme::warn, Icon::Warning); fy += 30; }
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
		// Three short values share the second row, so Controllers (the longest value: "Wii Remote, Nunchuk, Classic
		// Controller") gets the whole width of the third instead of being cut off at 4:3.
		const float col3 = rw / 3;
		info(ui, rx, iy, col3, "Downloads", formatCount(a.downloads));
		info(ui, rx + col3, iy, col3, "Works on", platforms(a));
		info(ui, rx + 2 * col3, iy, col3, "Size on SD", formatSize(a.uncompressedSize));
		iy += 42;
		info(ui, rx, iy, rw, "Controllers", peripherals(a));
		iy += 46;

		// Description panel with scrolling.
		Rect box{rx, iy, rw, bottom - iy};
		ui.g.fillRoundRect(box.inset(-1), 13, theme::border.withAlpha(0.6f));
		ui.g.fillRoundRect(box, 12, theme::panel);
		const int fs = 15;
		float lh = (float)ui.t.lineHeight(Font::Medium, fs);
		// The wrapped lines are keyed on the entry and the width. The wrap runs a few ms per frame (the live catalog has
		// descriptions of 27 KB), so the panel fills in over a few frames instead of stalling one.
		if (!wrap_ || wrappedFor_ != &a || wrappedW != (int)box.w) {
			wrappedFor_ = &a;
			wrappedW = (int)box.w;
			Text* text = &ui.t;
			const std::string& src = a.longDesc.empty() ? a.shortDesc : a.longDesc;
			wrap_ = std::make_unique<textwrap::Wrapper>([text, fs](std::string_view s) { return text->width(Font::Medium, fs, s); }, src,
			                                            wrappedW - 56);
			wrapMs_ = 0;
			wrapFrames_ = 0;
		}
		if (!wrap_->done()) {
			const Uint32 t0 = SDL_GetTicks();
			wrap_->step([t0] { return SDL_GetTicks() - t0 >= kWrapSliceMs; });
			wrapMs_ += SDL_GetTicks() - t0;
			wrapFrames_++;
			if (wrap_->done())
				LOG("Detail: wrapped %d bytes into %d lines in %u ms over %d frame(s)", (int)(a.longDesc.empty() ? a.shortDesc : a.longDesc).size(),
				    (int)wrap_->lines().size(), (unsigned)wrapMs_, wrapFrames_);
		}
		const std::vector<std::string>& lines = wrap_->lines();
		int visible = std::max(1, (int)((box.h - 20) / lh));
		int maxScroll = std::max(0, (int)lines.size() - visible);
		bool canScroll = maxScroll > 0;
		if (canScroll) {
			// Horizontal capsules 32x24 (the old circles were 26 across), clear of the text column (ends 40 px in).
			const float aw = 32, ah = 24, ax = box.right() - 18 - aw / 2;
			if (scroll > 0 && arrowButton(ui, makeId("detail:up"), {ax, box.y + 20 - ah / 2, aw, ah}, Icon::ChevronUp, 13))
				scroll -= visible - 1;
			if (scroll < maxScroll &&
			    arrowButton(ui, makeId("detail:down"), {ax, box.bottom() - 20 - ah / 2, aw, ah}, Icon::ChevronDown, 13))
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
	static constexpr Uint32 kWrapSliceMs = 6;
	std::unique_ptr<textwrap::Wrapper> wrap_;
	const AppEntry* wrappedFor_ = nullptr; // the entry and width the lines belong to
	int wrappedW = -1;
	Uint32 wrapMs_ = 0;
	int wrapFrames_ = 0;
	int scroll = 0;
};
} // namespace

std::unique_ptr<Screen> makeDetailScreen(const AppEntry& app) { return std::make_unique<DetailScreen>(app); }
