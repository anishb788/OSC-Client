#include "App.h"

#include <fstream>

#include "audio/Audio.h"
#include "core/Log.h"
#include "gfx/Theme.h"
#include "platform/Platform.h"
#include "ui/Widgets.h"

bool App::init(int argc, char** argv) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
		LOG("SDL_Init: %s", SDL_GetError());
		return false;
	}
	platform::init(argc, argv);
	if (!gfx.init(platform::widescreen(), 2)) return false;
	if (!text.init(gfx.renderer(), gfx.scaleX())) return false;
	audio::init(platform::dataDir());

	loadMockCatalog(catalog);
	LOG("Catalog: %d apps (mock data)", (int)catalog.apps.size());

	stack.push_back(makeHomeScreen());
	openDevStartScreen();
	return true;
}

// Development shortcut: `./dev run --screen detail` writes <dataDir>/devstart so the
// app opens straight on the screen being worked on.
void App::openDevStartScreen() {
	std::string which;
	if (const char* env = SDL_getenv("OPENSHOP_START")) which = env;
	std::ifstream f(platform::dataDir() + "/devstart");
	if (which.empty() && f) std::getline(f, which);
	if (which.empty() || which == "home") return;
	LOG("Dev start screen: %s", which.c_str());

	const std::vector<SortMode> sorts = {SortMode::Popular, SortMode::Newest, SortMode::Name};
	if (which == "search") stack.push_back(makeSearchScreen());
	else if (which == "list") stack.push_back(makeListScreen("Games", catalog.inCategory("games"), sorts, SortMode::Popular));
	else if (which == "detail" && catalog.featured()) stack.push_back(makeDetailScreen(*catalog.featured()));
	else if (which == "dialog") quitPrompt();
	else LOG("Unknown start screen '%s' (home, search, list, detail, dialog)", which.c_str());
}

void App::shutdown() {
	stack.clear();
	audio::shutdown();
	text.shutdown();
	gfx.shutdown();
	SDL_Quit();
}

void App::push(std::unique_ptr<Screen> s) {
	if (fadeDir == 1) return; // a transition is already in progress
	pendingPush = std::move(s);
	fadeDir = 1;
}

void App::pop() {
	if (fadeDir == 1 || stack.size() <= 1) return;
	audio::play(audio::Sfx::Back);
	pendingPop = true;
	fadeDir = 1;
}

void App::showDialog(Dialog d) {
	dialog = std::move(d);
	dialogAnim = 0;
}

void App::quitPrompt() {
	Dialog d;
	d.title = std::string(platform::exitLabel()) + " OpenShop?";
#ifdef __wii__
	d.message = "You will return to the Homebrew Channel.";
#else
	d.message = "The window will close.";
#endif
	d.buttons = {"Cancel", platform::exitLabel()};
	d.cancelIndex = 0;
	d.primaryIndex = 1;
	d.onChoose = [this](int i) { if (i == 1) quit(); };
	showDialog(std::move(d));
}

void App::run() {
	Uint64 last = SDL_GetPerformanceCounter();
	const double freq = (double)SDL_GetPerformanceFrequency();
	Input in;

	while (!quitRequested) {
		Uint64 now = SDL_GetPerformanceCounter();
		float dt = std::min(0.1f, (float)((now - last) / freq));
		last = now;

		platform::poll(in, dt, gfx.width());
		if (in.quit) break;

		// Transitions: fade to white, swap screens, fade back in.
		if (fadeDir != 0) {
			fade += fadeDir * dt * 7.0f;
			if (fadeDir == 1 && fade >= 1) {
				fade = 1;
				if (pendingPop && stack.size() > 1) stack.pop_back();
				if (pendingPush) stack.push_back(std::move(pendingPush));
				pendingPop = false;
				ui.resetFocusMemory();
				fadeDir = -1;
			} else if (fadeDir == -1 && fade <= 0) {
				fade = 0;
				fadeDir = 0;
			}
		}
		ui.inputEnabled = fadeDir == 0;
		ui.beginFrame(in, dt);

		if (ui.inputEnabled && (in.pressed & BTN_ONE) && audio::hasMusic()) audio::toggleMusic();
		if (ui.inputEnabled && !dialog && (in.pressed & BTN_HOME)) {
			audio::play(audio::Sfx::Select);
			quitPrompt();
		}

		gfx.beginFrame();
		gfx.background();
		stack.back()->draw(*this, ui);
		if (dialog) drawDialog();
		if (fade > 0) gfx.fillRect({0, 0, (float)gfx.width(), (float)gfx.height()}, theme::white.withAlpha(fade));
		if (in.pointerValid) gfx.cursor(in.px, in.py);
		gfx.endFrame();

		ui.endFrame();
		text.endFrame();
	}
}

void App::drawDialog() {
	Dialog& d = *dialog;
	float W = (float)gfx.width(), H = (float)gfx.height();
	dialogAnim = approach(dialogAnim, 1.0f, 14.0f, ui.dt);
	float a = easeOutCubic(dialogAnim);

	ui.pushLayer();
	gfx.fillRect({0, 0, W, H}, Color{30, 50, 70, (Uint8)(110 * a)});

	if (d.kind == Dialog::Kind::Progress) {
		d.progress = std::min(1.0f, d.progress + ui.dt * 0.4f);
		if (d.progress >= 1) {
			d.kind = Dialog::Kind::Message;
			d.title = d.doneTitle;
			d.message = d.doneMessage;
			d.buttons = {"OK"};
			d.primaryIndex = 0;
			audio::play(audio::Sfx::Select);
		}
	}

	float pw = std::min(W - 80, 480.0f);
	auto lines = text.wrap(Font::Medium, 17, d.message, (int)pw - 64, 6);
	float lh = (float)text.lineHeight(Font::Medium, 17);
	float bodyH = lines.size() * lh + (d.kind == Dialog::Kind::Progress ? 56 : 0);
	float ph = 34 + 34 + 12 + bodyH + 26 + 46 + 24;
	if (d.kind == Dialog::Kind::Progress) ph -= 46;
	float scale = 0.92f + 0.08f * a;
	Rect p{W / 2 - pw * scale / 2, H / 2 - ph * scale / 2 + (1 - a) * 12, pw * scale, ph * scale};

	gfx.softShadow(p, 22, 2.0f);
	gfx.roundRectBorder(p, 22, 2, theme::border, theme::card);
	float y = p.y + 30;
	text.drawIn(Font::Bold, 22, d.title, {p.x + 32, y, p.w - 64, 34}, theme::text, Align::Center);
	y += 34 + 12;
	for (auto& l : lines) {
		text.draw(Font::Medium, 17, l, p.cx(), y, theme::textMuted, Align::Center);
		y += lh;
	}

	if (d.kind == Dialog::Kind::Progress) {
		Rect bar{p.x + 40, y + 18, p.w - 80, 16};
		gfx.fillRoundRect(bar, 8, theme::panel);
		gfx.fillRoundRect(bar.inset(-1), 9, theme::border.withAlpha(0.5f));
		gfx.fillRoundRect(bar, 8, theme::panel);
		if (d.progress > 0.02f) gfx.fillRoundRect({bar.x, bar.y, bar.w * d.progress, bar.h}, 8, theme::accent);
		gfx.spinner(bar.cx(), bar.bottom() + 26, 10, ui.time, theme::accent);
		ui.popLayer();
		return;
	}

	int n = (int)d.buttons.size();
	float bw = std::min(170.0f, (p.w - 64 - (n - 1) * 16) / n);
	float bx = p.cx() - (n * bw + (n - 1) * 16) / 2;
	float by = p.bottom() - 24 - 46;
	int chosen = -1;
	for (int i = 0; i < n; i++) {
		Id id = makeId("dialog:button", (uint32_t)i);
		Pill style = i == d.primaryIndex ? Pill::Primary : Pill::Normal;
		if (pillButton(ui, id, {bx + i * (bw + 16), by, bw, 46}, d.buttons[i], style)) chosen = i;
		if (i == d.primaryIndex) ui.setDefaultFocus(id);
	}
	if (chosen < 0 && ui.pressed(BTN_BACK)) { chosen = d.cancelIndex; audio::play(audio::Sfx::Back); }
	ui.popLayer();

	if (chosen >= 0) {
		auto cb = std::move(d.onChoose);
		dialog.reset();
		if (cb) cb(chosen);
	}
}
