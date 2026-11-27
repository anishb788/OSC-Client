#include "App.h"
#include "core/DebugLog.h"

#include <cstdio>
#include <ctime>
#include <set>

#include <SDL_image.h>

#include "audio/Audio.h"
#include "catalog/InstallGuard.h"
#include "catalog/RefreshPolicy.h"
#include "core/Log.h"
#include "core/Version.h"
#include "gfx/Theme.h"
#include "net/HttpWii.h"
#include "platform/Platform.h"
#include "ui/Widgets.h"

#include <zip.h>

bool App::init(int argc, char** argv) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
		LOG("SDL_Init: %s", SDL_GetError());
		return false;
	}
	platform::init(argc, argv);
#ifdef OPENSHOP_DEBUGTOOLS
	debuglog::init(); // before the worker thread exists
	debuglog::openFile(platform::dataDir() + "/debuglog.txt");
	LOG("[debug] DEBUG BUILD (DEBUGTOOLS=1): every log line goes to the screen and to debuglog.txt");
#endif
	if (!gfx.init(platform::widescreen(), 2)) return false;
	// Corner masks for every radius the UI reaches, hover growth, focus glow and shadow spread included (the largest
	// is the 54 px Download capsule's shadow, 27 + 2 + 6). Built here once so hovering never creates textures mid-frame.
	gfx.prewarmCircles(40);
	if (!text.init(gfx.renderer(), gfx.scaleX())) return false;
	audio::init(platform::dataDir());

	opts = DevOptions::load(platform::dataDir());
	// The app stays usable without it, but submit() then returns 0 and nothing network-related can run.
	if (!worker_.start()) LOG("ERROR: the net worker failed to start; every network job will be rejected (worker().running() is false)");
	if (!opts.nettest.empty() || !opts.nettestBg.empty()) {
		std::string script = opts.nettestBg.empty() ? opts.nettest : "bg:" + opts.nettestBg + (opts.nettest.empty() ? "" : "," + opts.nettest);
		net::nettestLogTo(platform::dataDir() + "/nettest.txt");
		netTest_ = std::make_unique<net::NetSelfTest>(worker_, script, opts.nettestQuit);
	}
	{
		IconCacheConfig ic;
		ic.enabled = !opts.mock; // the mock catalog has no real icons: placeholders, and no network or SD use
		ic.network = !opts.offline;
		ic.cacheDir = platform::dataDir() + "/cache";
		ic.urlBase = opts.iconBase;
		icons_ = std::make_unique<IconCache>(&worker_, gfx.renderer(), std::move(ic));
		ui.icons = icons_.get();
	}
	LOG("OpenShop %s, User-Agent: %s", OPENSHOP_VERSION, userAgent());
	LOG("libzip %s", zip_libzip_version()); // proves the archive library is linked (M3 installs use it)
	LOG("Dev options: %s", opts.describe().c_str());
	{
		install::AppsDeviceSetting::Mounts m;
		for (const auto& r : platform::storageRoots()) {
			if (r.id == install::deviceId(install::Device::Sd)) { m.sd = r.mounted; m.sdRoot = r.root; }
			else if (r.id == install::deviceId(install::Device::Usb)) { m.usb = r.mounted; m.usbRoot = r.root; }
		}
		std::optional<install::Device> ov;
		if (!opts.appsDevice.empty()) {
			ov = install::parseDeviceId(opts.appsDevice);
			if (!ov) LOG("Apps device: dev option appsdevice=%s is not sd or usb, ignored", opts.appsDevice.c_str());
		}
		appsDevice_.setBusy([] {
			static bool logged = false;
			if (!logged) {
				logged = true;
				LOG("busy guard: no job system yet (Increment 7 plugs in Jobs::busy())");
			}
			return false;
		});
		appsDevice_.init(platform::dataDir() + "/appsdevice.ini", m, ov);
		if (opts.appsDeviceSwap)
			LOG("Apps device: appsdeviceswap=1 requested; it acts after the first scan (Increment 11), so nothing happens yet");
	}
	switch (opts.probeChoice()) { // one probe per launch
		case DevOptions::ProbeChoice::Heap: platform::heapProbe(); break;
		case DevOptions::ProbeChoice::Stack: platform::stackProbe(); break;
		case DevOptions::ProbeChoice::Conflict:
			LOG("probe: heapprobe and stackprobe were both requested; they disturb each other, so NEITHER runs. Ask for one.");
			break;
		case DevOptions::ProbeChoice::None: break;
	}
	// A release must not behave differently because of a stray devstart file or environment variable, so say so loudly.
	// (Compile-time gating of these hooks is a later item; this is only the warning.)
	if (const std::string active = opts.active(); !active.empty())
		LOG("WARNING: DEV OPTIONS ACTIVE, this is not a normal launch: %s", active.c_str());
	// Only the failure is logged: a normal launch writes nothing to the SD card but the catalog and icon caches.
	if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) LOG("SDL2_image: IMG_Init(PNG) failed: %s", IMG_GetError());

	LoaderOptions startup = LoaderOptions::from(opts);
	startup.worker = &worker_;
	startup.cacheDir = platform::dataDir() + "/cache";
	auto loading = makeLoadingScreen(startup);
	loads.recordStartupScreen(loading.get());
	stack.push_back(std::move(loading));
	return true;
}

// Development shortcut: `./dev run --screen detail` writes <dataDir>/devstart (see DevOptions)
// so the app opens straight on the screen being worked on.
void App::openDevStartScreen() {
	const std::string& which = opts.screen;
#ifdef OPENSHOP_DEBUGTOOLS
	if (which.empty()) {
		stack.push_back(makeNetTestScreen());
		return;
	}
#endif
	if (which.empty() || which == "home") return;
	LOG("Dev start screen: %s", which.c_str());

	if (which == "search") stack.push_back(makeSearchScreen());
	else if (which == "list") stack.push_back(makeListScreen("Games", catalog.inCategory("games"), kAllSorts, SortMode::Popular));
	else if (which == "detail" && catalog.featured()) stack.push_back(makeDetailScreen(*catalog.featured()));
	else if (which == "dialog") quitPrompt();
	else LOG("Unknown start screen '%s' (home, search, list, detail, dialog)", which.c_str());
}

void App::shutdown() {
	// First, while the SDL timer and threads still exist: cancels and joins any in-flight transfer.
	uint32_t stopT0 = SDL_GetTicks();
	worker_.stop();
	// Results the main thread never got to drain: their captures are destroyed now, while SDL is still up, not in ~Worker
	// after SDL_Quit().
	worker_.discardResults();
	if (netTest_) NTLOG("[nettest] net worker stopped in %u ms", SDL_GetTicks() - stopT0);
	// Before SDL goes away; skips net_deinit (and says why) when init or a DNS lookup is still live in IOS. No-op on PC.
	const char* netDown = net::shutdown();
	if (netTest_) NTLOG("[nettest] net shutdown: %s", netDown);
	if (netTest_) net::nettestLogTo("");
#ifdef OPENSHOP_DEBUGTOOLS
	LOG("[debug] worker stopped in %u ms; net shutdown: %s; exiting", SDL_GetTicks() - stopT0, netDown);
	debuglog::closeFile();
#endif
	stack.clear();
	// Textures must go before the renderer does (Gfx::shutdown below).
	ui.icons = nullptr;
	icons_.reset();
	IMG_Quit();
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

bool App::resetTo(std::unique_ptr<Screen> s, bool openDevStart) {
	if (fadeDir == 1) return false; // a transition is already fading out
	pendingReset = std::move(s);
	resetOpensDevStart = openDevStart;
	fadeDir = 1;
	return true;
}

uint32_t App::beginLoad(const Screen* from) { return loads.beginLoad(from); }

bool App::installCatalog(Catalog&& c, uint32_t token, bool fetchedFromNetwork) {
	const void* only = stack.size() == 1 ? stack.front().get() : nullptr;
	if (const char* why = loads.refusal(only, pendingPush || pendingReset || pendingPop, token)) {
		LOG("installCatalog refused: %s", why);
		return false;
	}
	catalog_ = std::move(c);
	loads.accepted(SDL_GetTicks(), fetchedFromNetwork);
	LOG("installCatalog accepted, generation %u: %d apps%s", loads.generation(), (int)catalog_.apps.size(),
	    fetchedFromNetwork ? "" : " (not a network fetch)");
	return true;
}

bool App::refreshCatalog() {
	if (!loads.installed()) { LOG("Refresh ignored: no catalog is installed yet"); return false; }
	if (!canChangeScreen()) { LOG("Refresh ignored: a screen change is in progress"); return false; }
	if (dialog) { LOG("Refresh ignored: a dialog is open"); return false; }
	LoaderOptions lo = LoaderOptions::from(opts);
	lo.mode = LoaderOptions::Mode::Refresh;
	lo.worker = &worker_;
	lo.cacheDir = platform::dataDir() + "/cache";
	int limit = opts.rateLimitSec >= 0 ? opts.rateLimitSec : kRefreshMinIntervalSec;
	// The fail=refresh dev hook would never fire inside the first minute otherwise; an explicit ratelimit= wins.
	if (opts.failRefresh && opts.rateLimitSec < 0) limit = 0;
	const int64_t since = loads.msSinceFetch(SDL_GetTicks());
	if (refreshRateLimited(since, limit)) {
		lo.reuseCurrent = true;
		LOG("Refresh: last fetch %lld s ago (< %d s), skipping the network", (long long)(since / 1000), limit);
	} else {
		LOG("Refresh: fetching the catalog (generation %u so far)", loads.generation());
	}
	auto screen = makeLoadingScreen(lo);
	const Screen* raw = screen.get();
	if (!resetTo(std::move(screen), false)) return false;
	loads.setPendingScreen(raw);
	return true;
}

// Once, after the first catalog is installed: icons of apps that are no longer in it are deleted in the background
// (never after a Refresh, and never with the mock catalog, whose slugs would wipe the real cache).
void App::startIconPrune() {
	iconsPruned_ = true;
	std::set<std::string> keep;
	for (const AppEntry& a : catalog_.apps) keep.insert(a.slug);
	icons_->startPrune(keep);
}

// Dev hook (autorefresh=): once Home alone has been up for autoRefreshMs, press Refresh for the user.
void App::autoRefreshTick() {
	const bool homeAlone = loads.installed() && stack.size() == 1 && stack.front().get() != loads.recordedScreen();
	if (!homeAlone || pendingPush || pendingReset || pendingPop) {
		homeSinceMs_ = 0;
		return;
	}
	const uint32_t now = SDL_GetTicks();
	if (!homeSinceMs_) homeSinceMs_ = now ? now : 1;
	if (now - homeSinceMs_ < (uint32_t)opts.autoRefreshMs) return;
	LOG("autorefresh: pressing Refresh (generation %u)", loads.generation());
	// Either way, wait a full interval before the next press: a refused Refresh (a dialog is open, e.g. the
	// "Couldn't refresh" notice nobody can click away in Dolphin) would otherwise retry, and log, every frame.
	refreshCatalog();
	homeSinceMs_ = 0;
}

void App::showDialog(Dialog d) {
	// Every new dialog starts with fresh focus. This matters when one dialog replaces another (e.g. opened from an
	// onChoose/onCancel callback, which runs after the old dialog is reset): the modal depth stays 1, so Ui sees no
	// layer change and would otherwise keep the old dialog's button focused.
	ui.clearFocus();
	dialog = std::move(d);
	dialogAnim = 0;
}

void App::dropDialog() {
	if (!dialog) return;
	std::function<void()> cb;
	if (dialog->kind == Dialog::Kind::Progress) cb = std::move(dialog->onCancel);
	dialog.reset();
	if (cb) cb();
	dialog.reset(); // a callback must not resurrect a dialog across a reset
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
	d.pausesMusic = true;
	d.onChoose = [this](int i) { if (i == 1) quit(); };
	audio::play(audio::Sfx::Home);
	showDialog(std::move(d));
}

void App::comingSoon(const char* what) {
	Dialog d;
	d.title = "Coming soon";
	d.message = std::string(what) + " is not available yet.";
	d.primaryIndex = 0;
	showDialog(std::move(d));
}

void App::openDownloads() { comingSoon("Downloads"); }
void App::openSettings() { comingSoon("Settings"); }

void App::openMyApps() {
	Dialog d;
	d.title = "Coming soon";
	d.message = "My apps is coming soon. Apps you install and their updates will appear here.";
	d.primaryIndex = 0;
	showDialog(std::move(d));
}

void App::topBar(Ui& ui, std::string_view title, std::string_view subtitle,
                 std::initializer_list<TopBarAction> hide) {
	TopBarButtons buttons;
	buttons[TopBarAction::Refresh].visible = true; // Downloads (M3) and Settings (M4) stay hidden until they exist
	if (opts.topbarAll) {
		for (auto& b : buttons.b) b.visible = true;
		// Fake state so the preview shows the active outline, progress strip and count badge.
		buttons[TopBarAction::Downloads].active = true;
		buttons[TopBarAction::Downloads].progress = 0.42f;
		buttons[TopBarAction::Downloads].badge = Badge{"2", theme::accent};
	}
	for (TopBarAction a : hide) buttons[a].visible = false;
	auto act = drawTopBar(ui, title, subtitle, &buttons);
	if (!act) return;
	switch (*act) {
	case TopBarAction::Downloads: openDownloads(); break;
	case TopBarAction::Refresh: (void)refreshCatalog(); break;
	case TopBarAction::Settings: openSettings(); break;
	}
}

void App::run() {
	Uint64 last = SDL_GetPerformanceCounter();
	const double freq = (double)SDL_GetPerformanceFrequency();
	Input in;
	// perf=1 bookkeeping: sums over the last 120 frames, in ms (draw = beginFrame to just before present).
	double perfCpu = 0, perfPresent = 0, perfFrame = 0, perfMax = 0;
	int perfFrames = 0;

	while (!quitRequested) {
		Uint64 now = SDL_GetPerformanceCounter();
		float dt = std::min(0.1f, (float)((now - last) / freq));
		last = now;
		perfFrame += dt * 1000.0;
		perfMax = std::max(perfMax, (double)dt * 1000.0);

		platform::poll(in, dt, gfx.width());
		if (in.quit) break;
		worker_.drainResults();
		if (opts.autoRefreshMs >= 0) autoRefreshTick();
#ifdef OPENSHOP_DEBUGTOOLS
		debuglog::pump();
#endif
		if (netTest_ && netTest_->tick(dt)) quitRequested = true;

		// Transitions: fade to white, swap screens, fade back in.
		if (fadeDir != 0) {
			fade += fadeDir * dt * 7.0f;
			if (fadeDir == 1 && fade >= 1) {
				fade = 1;
				if (pendingReset) {
					dropDialog();
					stack.clear();
					stack.push_back(std::move(pendingReset));
					// Only the screen refreshCatalog() handed to resetTo becomes the recorded LoadingScreen.
					loads.onStackReset(stack.front().get());
					pendingPush.reset();
					pendingPop = false;
					if (resetOpensDevStart) openDevStartScreen();
					resetOpensDevStart = false;
					LOG("Screen stack reset: %d screen(s)%s", (int)stack.size(), loads.recordedScreen() ? " (the LoadingScreen)" : "");
				}
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
		ui.setModalDepth(dialog ? 1 : 0);
		ui.beginFrame(in, dt);
		icons_->beginFrame(ui.time, loads.generation());
		if (!iconsPruned_ && loads.generation() > 0 && !opts.mock) startIconPrune();

		if (ui.inputEnabled && (in.pressed & BTN_ONE) && audio::hasMusic()) audio::toggleMusic();
		if (ui.inputEnabled && !dialog && (in.pressed & BTN_HOME) && !stack.back()->handleHome(*this)) quitPrompt();
		audio::setMusicSuspended(dialog && dialog->pausesMusic);

		Uint64 drawStart = SDL_GetPerformanceCounter();
		gfx.beginFrame();
		gfx.background();
		stack.back()->draw(*this, ui);
		if (dialog) drawDialog();
		if (fade > 0) gfx.fillRect({0, 0, (float)gfx.width(), (float)gfx.height()}, theme::white.withAlpha(fade));
		if (in.pointerValid) gfx.cursor(in.px, in.py);
		Uint64 beforePresent = SDL_GetPerformanceCounter();
		gfx.endFrame();

		text.endFrame();
		icons_->endFrame();
		if (opts.perf) {
			Uint64 end = SDL_GetPerformanceCounter();
			perfCpu += (double)(beforePresent - drawStart) * 1000.0 / freq;
			perfPresent += (double)(end - beforePresent) * 1000.0 / freq;
			if (++perfFrames == 120) {
				const IconCache::Stats is = icons_->stats();
				LOG("perf: frame %.2f ms (worst %.1f), draw %.2f ms, present %.2f ms, masks created %d (%d frames); icons %zu textures %zu KB, text %zu textures %zu KB",
				    perfFrame / 120, perfMax, perfCpu / 120, perfPresent / 120, gfx.masksCreated(), perfFrames, is.textures,
				    is.textureBytes / 1024, text.cacheCount(), text.cacheBytes() / 1024);
				LOG("perf: %s", platform::heapSummary().c_str());
				perfCpu = perfPresent = perfFrame = perfMax = 0;
				perfFrames = 0;
			}
		}
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
			d.cancelIndex = 0;
			d.onCancel = nullptr; // the work finished; only onChoose(0) is left to report
			audio::play(audio::Sfx::Select);
		}
	}

	float pw = std::min(W - 80, 480.0f);
	auto lines = text.wrap(Font::Medium, 17, d.message, (int)pw - 64, 6);
	float lh = (float)text.lineHeight(Font::Medium, 17);
	float bodyH = lines.size() * lh + (d.kind == Dialog::Kind::Progress ? 56 : 0);
	float ph = 34 + 34 + 12 + bodyH + 26 + 46 + 24;
	if (d.kind == Dialog::Kind::Progress && !d.cancellable) ph -= 46;
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
		if (d.cancellable) {
			Id id = makeId("dialog:cancel");
			bool cancel = pillButton(ui, id, {p.cx() - 85, p.bottom() - 24 - 46, 170, 46}, "Cancel");
			ui.setDefaultFocus(id);
			if (!cancel && ui.pressed(BTN_BACK)) { cancel = true; audio::play(audio::Sfx::Back); }
			if (cancel) {
				auto cb = std::move(d.onCancel);
				ui.popLayer();
				dialog.reset();
				if (cb) cb();
				return;
			}
		}
		ui.popLayer();
		return;
	}

	int n = (int)d.buttons.size();
	float bw = std::min(170.0f, (p.w - 64 - (n - 1) * 16) / n);
	float bx = p.cx() - (n * bw + (n - 1) * 16) / 2;
	float by = p.bottom() - 24 - 46;
	int chosen = -1;
	// The D-pad's starting point: the primary button, else the cancel button, else the first one. Never the
	// destructive button: pressing A at once must not delete anything. Set explicitly so Ui's own fallback (the first
	// item on the layer) can never land on it.
	auto safe = [&](int i) { return i >= 0 && i < n && i != d.destructiveIndex; };
	int start = safe(d.primaryIndex) ? d.primaryIndex : safe(d.cancelIndex) ? d.cancelIndex : -1;
	for (int i = 0; start < 0 && i < n; i++) if (safe(i)) start = i;
	for (int i = 0; i < n; i++) {
		Id id = makeId("dialog:button", (uint32_t)i);
		Pill style = i == d.destructiveIndex ? Pill::Destructive : i == d.primaryIndex ? Pill::Primary : Pill::Normal;
		if (pillButton(ui, id, {bx + i * (bw + 16), by, bw, 46}, d.buttons[i], {.style = style})) chosen = i;
		if (i != start) continue;
		ui.setDefaultFocus(id);
		// A non-destructive primary is focused straight away for D-pad / keyboard users (A answers at once). Only
		// when no pointer is aimed and nothing is focused yet, so it never fights the Wii Remote.
		if (i == d.primaryIndex) ui.focusForDpad(id);
	}
	if (chosen < 0 && ui.pressed(BTN_BACK)) { chosen = d.cancelIndex; audio::play(audio::Sfx::Back); }
	ui.popLayer();

	if (chosen >= 0) {
		auto cb = std::move(d.onChoose);
		dialog.reset();
		if (cb) cb(chosen);
	}
}
