#include "App.h"
#include "audio/Audio.h"
#include "catalog/CatalogLoader.h"
#include "core/Log.h"
#include "core/Version.h"
#include "gfx/Theme.h"
#include "platform/Platform.h"
#include "ui/Widgets.h"

namespace {
// Startup screen: spinner + status while the loader works, then installs the catalog and hands over to Home
// (and the --screen target). If the load fails it shows the reason with Retry and Exit.
//
// Refresh mode (opts.mode == Refresh) looks the same, with "Press B to cancel" and no error UI. It always ends on
// [Home]: success installs the new catalog (or, when the Refresh was rate limited, keeps the current one), failure
// and B keep the previous catalog and add a "Couldn't refresh" message (failure only) on Home.
class LoadingScreen : public Screen {
public:
	explicit LoadingScreen(const LoaderOptions& opts)
	    : opts_(opts), refresh_(opts.mode == LoaderOptions::Mode::Refresh) {}

	// On the error screen HOME exits straight away (no confirmation): there is nothing to lose there. Otherwise
	// (loading, also during a Refresh) App opens the exit menu and the load keeps running underneath.
	bool handleHome(App& app) override {
		if (!failed_) return false;
		LOG("HOME on the error screen: exiting");
		app.quit();
		return true;
	}

	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();

		if (!loader_ && !failed_ && !ready_) startLoad(app);

		// B cancels a Refresh (there is nothing to go back to on the startup load). The loader is destroyed, which
		// cancels its work; the previous catalog stays installed and Home follows without a dialog. ui.pressed is
		// false under a dialog (the HOME menu) and during fades.
		if (refresh_ && !ready_ && !failed_ && ui.pressed(BTN_BACK)) {
			LOG("Refresh cancelled with B; the previous catalog (generation %u) stays", app.catalogGeneration());
			audio::play(audio::Sfx::Back);
			loader_.reset();
			ready_ = true;
		}

		if (loader_ && !ready_ && !failed_) {
			// Longest gap between two frames while loading: a jansson parse on the worker must not stall rendering
			// (the worker is a low-priority thread, but on the single-core Wii it still takes CPU).
			const Uint32 now = SDL_GetTicks();
			if (lastFrameMs_) maxFrameMs_ = std::max(maxFrameMs_, now - lastFrameMs_);
			lastFrameMs_ = now;
			LoadStatus st = loader_->poll();
			if (st.state != LoadStatus::State::Pending)
				LOG("Catalog load over: longest frame while loading %u ms", (unsigned)maxFrameMs_);
			if (st.state == LoadStatus::State::Pending) {
				if (!st.text.empty()) status_ = st.text;
			} else if (st.state == LoadStatus::State::Failed) {
				LOG("Catalog %s failed: %s", refresh_ ? "refresh" : "load", st.text.c_str());
				fail(app, st.text);
			} else if (st.state == LoadStatus::State::Unchanged) {
				LOG("Refresh: keeping the installed catalog (generation %u)", app.catalogGeneration());
				loader_.reset();
				ready_ = true;
			} else if (app.installCatalog(loader_->take(), token_, st.fromNetwork)) {
				loader_.reset();
				ready_ = true;
			} else {
				fail(app, "Internal error: the catalog could not be installed.");
			}
		}
		// Hand over to Home once no other transition is running (checked first, so Home is only built once) and
		// no dialog is open: the HOME menu stays up until the user answers it instead of vanishing in the reset.
		// A Refresh never opens the --screen target, so the stack is exactly [Home].
		if (ready_ && !handedOver_ && !app.dialogOpen() && app.canChangeScreen()) {
			// notice_ is copied, not moved: if the reset were refused it must survive for the next attempt.
			handedOver_ = refresh_ ? app.resetTo(makeHomeScreen(notice_), false) : app.resetTo(makeHomeScreen(), true);
		}

		const float cx = W / 2;
		ui.t.draw(Font::Bold, 40, "OpenShop", cx, H * 0.17f, theme::text, Align::Center);
		ui.t.draw(Font::Medium, 18, "Open Shop Channel client", cx, H * 0.17f + 52, theme::textMuted, Align::Center);

		if (failed_) {
			drawError(app, ui, W, H);
		} else {
			const float sy = H * 0.52f;
			ui.g.spinner(cx, sy, 30, ui.time, theme::accent);
			ui.t.drawIn(Font::Medium, 18, status_, {cx - 240, sy + 52, 480, 28}, theme::textMuted, Align::Center);
			// The info-chip slot (sy + 90, 32 px high) stays empty in M2. M3 fills it with
			// "<App> keeps downloading in the background".
			if (refresh_) drawCancelHint(ui, cx, sy + 90 + 32 + 14);
		}

		ui.t.drawIn(Font::Medium, 14, "Catalog by the Open Shop Channel, OSC Team, Repository Manager",
		            {theme::margin, H - 48, W - 2 * theme::margin, 24}, theme::textMuted, Align::Center);
	}

private:
	void startLoad(App& app) {
		token_ = app.beginLoad(this);
		loader_ = makeCatalogLoader(opts_, attempt_++);
		status_ = refresh_ ? "Checking for new and updated apps…" : "Loading catalog…";
		failed_ = false;
		lastFrameMs_ = maxFrameMs_ = 0;
	}

	// Startup: the error screen. Refresh: keep the old catalog and go Home with a message.
	void fail(App& app, const std::string& reason) {
		if (!refresh_) {
			failed_ = true;
			error_ = reason;
			return;
		}
		notice_ = RefreshNotice{reason, formatDateTime(app.catalog.fetchedAt)};
		loader_.reset();
		ready_ = true;
	}

	// "Press [B] to cancel", with B on a little key cap.
	void drawCancelHint(Ui& ui, float cx, float y) {
		const int size = 14;
		const float cap = 22, gap = 7;
		float w1 = (float)ui.t.width(Font::Medium, size, "Press"), w2 = (float)ui.t.width(Font::Medium, size, "to cancel");
		float x = cx - (w1 + gap + cap + gap + w2) / 2;
		ui.t.drawIn(Font::Medium, size, "Press", {x, y, w1 + 2, cap}, theme::textMuted);
		Rect k{x + w1 + gap, y, cap, cap};
		ui.g.fillRoundRect({k.x, k.y + 2, k.w, k.h}, 6, theme::border);
		ui.g.roundRectBorder(k, 6, 1.5f, theme::border, theme::card);
		ui.t.drawIn(Font::Bold, 13, "B", k, theme::text, Align::Center);
		ui.t.drawIn(Font::Medium, size, "to cancel", {k.right() + gap, y, w2 + 2, cap}, theme::textMuted);
	}

	void drawError(App& app, Ui& ui, float W, float H) {
		const float cx = W / 2;
		ui.t.draw(Font::Bold, 26, "Could not load the catalog", cx, H * 0.40f, theme::text, Align::Center);
		auto lines = ui.t.wrap(Font::Medium, 18, error_, (int)W - 160, 4);
		float lh = (float)ui.t.lineHeight(Font::Medium, 18);
		float y = H * 0.40f + 48;
		for (auto& l : lines) {
			ui.t.draw(Font::Medium, 18, l, cx, y, theme::textMuted, Align::Center);
			y += lh;
		}
		float by = std::max(y + 28, H * 0.62f), bw = 170, gap = 20;
		Id retry = makeId("loading:retry");
		if (pillButton(ui, retry, {cx - bw - gap / 2, by, bw, 48}, "Retry", {.style = Pill::Primary, .fontSize = 19})) {
			LOG("Retrying the catalog load");
			startLoad(app);
		}
		ui.setDefaultFocus(retry);
		Id exit = makeId("loading:exit");
		// D-pad / keyboard users can press A straight away. focusForDpad only acts when nothing is focused, no
		// pointer is aimed and no dialog or fade is on top, so it never fights the user or the HOME dialog.
		ui.focusForDpad(retry);
		if (pillButton(ui, exit, {cx + gap / 2, by, bw, 48}, platform::exitLabel(), {.fontSize = 19}))
			app.quit();
	}

	LoaderOptions opts_;
	bool refresh_;
	int attempt_ = 0;
	uint32_t token_ = 0; // App::beginLoad's token for the current loader
	std::unique_ptr<CatalogLoader> loader_;
	std::string status_, error_;
	std::optional<RefreshNotice> notice_;
	// failed_: the startup error screen is showing. ready_: the load is over (installed, reused, failed in refresh
	// mode, or cancelled) and only the handover to Home is left. handedOver_: that reset has been accepted.
	bool failed_ = false, ready_ = false, handedOver_ = false;
	Uint32 lastFrameMs_ = 0, maxFrameMs_ = 0;
};
} // namespace

std::unique_ptr<Screen> makeLoadingScreen(const LoaderOptions& opts) { return std::make_unique<LoadingScreen>(opts); }
