#include "App.h"
#include "catalog/CatalogLoader.h"
#include "core/Log.h"
#include "core/Version.h"
#include "gfx/Theme.h"
#include "platform/Platform.h"
#include "ui/Widgets.h"

namespace {
// Startup screen: spinner + status while the loader works, then installs the catalog and hands over to Home.
// If the load fails it shows the reason with Retry and Exit.
class LoadingScreen : public Screen {
public:
	explicit LoadingScreen(const LoaderOptions& opts) : opts_(opts) { startLoad(); }

	// On the error screen HOME exits straight away (no confirmation): there is nothing to lose there.
	bool handleHome(App& app) override {
		if (!failed_) return false;
		LOG("HOME on the error screen: exiting");
		app.quit();
		return true;
	}

	void draw(App& app, Ui& ui) override {
		float W = (float)ui.g.width(), H = (float)ui.g.height();

		if (!failed_ && !done_) {
			LoadStatus st = loader_->poll();
			if (st.state == LoadStatus::State::Pending) {
				status_ = st.text;
			} else if (st.state == LoadStatus::State::Failed) {
				failed_ = true;
				error_ = st.text;
				LOG("Catalog load failed: %s", error_.c_str());
			} else if (app.installCatalog(loader_->take())) {
				done_ = true;
				loader_.reset();
			} else {
				failed_ = true;
				error_ = "Internal error: the catalog could not be installed.";
			}
		}
		// Hand over to Home once no other transition is running (checked first, so Home is only built once) and
		// no dialog is open: the HOME menu stays up until the user answers it instead of vanishing in the reset.
		if (done_ && !handedOver_ && !app.dialogOpen() && app.canChangeScreen())
			handedOver_ = app.resetTo(makeHomeScreen(), true);

		const float cx = W / 2;
		ui.t.draw(Font::Bold, 44, "OpenShop", cx, 92, theme::text, Align::Center);
		ui.t.draw(Font::Medium, 18, "Open Shop Channel client", cx, 148, theme::textMuted, Align::Center);

		if (failed_) {
			drawError(app, ui, W, H);
		} else {
			ui.g.spinner(cx, H * 0.52f, 30, ui.time, theme::accent);
			ui.t.drawIn(Font::Medium, 18, status_, {cx - 240, H * 0.52f + 52, 480, 28}, theme::textMuted, Align::Center);
		}

		ui.t.drawIn(Font::Medium, 14, "Catalog by the Open Shop Channel, OSC Team, Repository Manager",
		            {theme::margin, H - 48, W - 2 * theme::margin, 24}, theme::textMuted, Align::Center);
	}

private:
	void startLoad() {
		loader_ = makeCatalogLoader(opts_, attempt_++);
		status_ = "Loading catalog…";
		failed_ = false;
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
		if (pillButton(ui, retry, {cx - bw - gap / 2, by, bw, 48}, "Retry", Pill::Primary, nullptr, 19)) {
			LOG("Retrying the catalog load");
			startLoad();
		}
		ui.setDefaultFocus(retry);
		Id exit = makeId("loading:exit");
		// D-pad / keyboard users can press A straight away. focusForDpad only acts when nothing is focused, no
		// pointer is aimed and no dialog or fade is on top, so it never fights the user or the HOME dialog.
		ui.focusForDpad(retry);
		if (pillButton(ui, exit, {cx + gap / 2, by, bw, 48}, platform::exitLabel(), Pill::Normal, nullptr, 19))
			app.quit();
	}

	LoaderOptions opts_;
	int attempt_ = 0;
	std::unique_ptr<CatalogLoader> loader_;
	std::string status_, error_;
	bool failed_ = false, done_ = false, handedOver_ = false;
};
} // namespace

std::unique_ptr<Screen> makeLoadingScreen(const LoaderOptions& opts) { return std::make_unique<LoadingScreen>(opts); }
