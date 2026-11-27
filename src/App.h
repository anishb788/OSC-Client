#pragma once
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/Catalog.h"
#include "catalog/CatalogLoader.h"
#include "catalog/LoadTracker.h"
#include "core/DevOptions.h"
#include "gfx/Gfx.h"
#include "gfx/IconCache.h"
#include "gfx/Text.h"
#include "install/AppsDevice.h"
#include "net/NetSelfTest.h"
#include "net/Worker.h"
#include "ui/Ui.h"
#include "ui/Widgets.h"

class App;

class Screen {
public:
	virtual ~Screen() = default;
	virtual void draw(App& app, Ui& ui) = 0;
	// HOME was pressed while this screen is on top and no dialog is open. Return true if the screen handled it;
	// otherwise App opens the exit menu.
	virtual bool handleHome(App&) { return false; }
};

struct Dialog {
	enum class Kind { Message, Progress };
	Kind kind = Kind::Message;
	std::string title, message;
	std::vector<std::string> buttons{"OK"};
	int cancelIndex = 0;                 // Message dialogs: the button B / Back chooses
	int primaryIndex = -1;               // drawn as the blue button
	int destructiveIndex = -1;           // drawn as the red button (confirming a deletion); wins over primaryIndex
	// Called with the chosen button's index when a Message dialog closes, including the "done" message a
	// Progress dialog turns into when it finishes. Never called for a cancel; see onCancel.
	std::function<void(int)> onChoose;
	// Progress dialogs (preview only for now): message shown when finished.
	std::string doneTitle, doneMessage;
	float progress = 0;
	// Progress dialogs: show a Cancel pill (and let B cancel). Cancelling closes the dialog and calls onCancel,
	// never onChoose. onCancel is also called if the dialog is dropped by App::resetTo while still in progress,
	// so whoever started the work always hears that it should stop.
	bool cancellable = false;
	std::function<void()> onCancel;
	// Background music is paused while this dialog is open and resumes when it closes (by any path).
	bool pausesMusic = false;
};

class App {
public:
	App() = default;
	// `catalog` and `ui` are references into this object, so a copy would point at the original.
	App(const App&) = delete;
	App& operator=(const App&) = delete;

	Gfx gfx;
	Text text;
	Ui ui{gfx, text};
	// The catalog is read-only for everyone except installCatalog().
	//
	// INVARIANT: it is replaced only while the LoadingScreen App recorded for the current load is the only screen
	// and no screen change is pending (catalogInstallRefusal), at startup and on every Refresh. Every holder of
	// `const AppEntry&` / `const AppEntry*` is a Screen (DetailScreen holds a `const AppEntry&`, ListScreen and
	// SearchScreen hold `const AppEntry*` lists, HomeScreen only borrows `featured()` within a frame), and the reset
	// to the LoadingScreen destroys all of them first, so nothing can dangle. Background work (worker jobs, the
	// IconCache, M3's installs) never holds catalog pointers: it copies what it needs and refers to apps by slug.
	// `catalog` is a const reference so the compiler enforces the read-only part. catalogGeneration() counts the
	// accepted installs, for caches keyed on catalog content.
	//
	// A refresh that keeps the user's place (out of scope) would need the alternatives instead: every holder stores
	// a slug and re-resolves with `catalog.find(slug)` each draw (pop if it is gone), or screens hold
	// `std::shared_ptr<const Catalog>` snapshots so old screens keep the catalog they were built from alive.
	const Catalog& catalog{catalog_};
	DevOptions opts; // parsed once in init()

	// The one network worker (started in init(), stopped first thing in shutdown()). The catalog loader submits to it. Its
	// rules, including "jobs never hold catalog pointers", are in net/Worker.h. If start() failed, running() is false
	// and submit() returns 0: callers must show an error instead of waiting on a job that will never run.
	net::Worker& worker() { return worker_; }
	// App icons (Increment 11): textures keyed by slug, fed by Icon jobs on the worker. Null before init() and after
	// shutdown(); Ui::icons points at it for the widgets.
	IconCache* icons() { return icons_.get(); }

	bool init(int argc, char** argv);
	void run();
	void shutdown();

	void push(std::unique_ptr<Screen> s);
	void pop();
	// False while a transition is fading out, when push/pop/resetTo would be ignored or refused.
	bool canChangeScreen() const { return fadeDir != 1; }
	bool dialogOpen() const { return dialog.has_value(); }
	// Fades to white, drops every screen (and any open dialog, calling a Progress dialog's onCancel), shows `s`
	// as the only screen and fades in. With openDevStart the --screen target is pushed on top of it while the
	// screen is still white. Returns false (nothing happens, `s` is discarded) while another transition is
	// fading out; check canChangeScreen() first or retry next frame.
	[[nodiscard]] bool resetTo(std::unique_ptr<Screen> s, bool openDevStart = false);
	// The only way to set the catalog. Refuses (LOG, returns false) unless the recorded LoadingScreen is the only
	// screen, no screen change is pending and `token` is the one beginLoad() last returned (see LoadTracker /
	// catalogInstallRefusal). Each accepted install bumps catalogGeneration(). `fetchedFromNetwork` is false for a
	// catalog served from the SD cache (Increment 10): only a network fetch restarts the Refresh rate limit.
	[[nodiscard]] bool installCatalog(Catalog&& c, uint32_t token, bool fetchedFromNetwork);
	// The LoadingScreen calls this each time it starts a load (first draw, Retry) and passes the token to
	// installCatalog, so a loader that a Retry replaced can't install. `from` must be the calling screen.
	uint32_t beginLoad(const Screen* from);
	// 0 before the first install, then +1 for every accepted install.
	uint32_t catalogGeneration() const { return loads.generation(); }
	// The Refresh capsule's action: reloads the catalog through the LoadingScreen and lands on [Home] (no --screen
	// target). Does nothing (and logs why) unless a catalog is installed, no transition is fading out and no dialog
	// is open. A Refresh within kRefreshMinIntervalSec of the last successful fetch skips the network but takes the
	// same route. Returns true if the refresh started.
	bool refreshCatalog();
	// "Show apps on" (Increment 2b). The root of the device apps are shown from and installed to this session ("" = no SD
	// card and no USB drive: installs are refused). Replaces any argv[0]-derived install root. By value: never concatenate
	// onto an empty root (check hasAppsDevice()), and copy it into the JobSpec at queue time.
	std::string appsRoot() const { return appsDevice_.root(); }
	bool hasAppsDevice() const { return appsDevice_.hasAppsDevice(); }
	unsigned appsDeviceEpoch() const { return appsDevice_.epoch(); }
	install::Device appsDevice() const { return appsDevice_.resolved(); }
	install::Device savedAppsDevice() const { return appsDevice_.saved(); }
	install::Fallback appsDeviceFallback() const { return appsDevice_.fallback(); }
	// Saves the choice and switches to it. Refused (with a UI-ready `reason`) while a job is queued or running (Increment 7
	// replaces the busy placeholder with Jobs::busy) or when the device is not mounted. SwitchedNotSaved: switched for this
	// session but the save failed (UI: "Couldn't remember this choice"). Increment 11 adds the re-scan. Main thread only.
	install::AppsDeviceSetting::SetResult setAppsDevice(install::Device dev, std::string* reason = nullptr) {
		return appsDevice_.set(dev, reason);
	}
	install::AppsDeviceSetting& appsDeviceSetting() { return appsDevice_; }
	void quit() { quitRequested = true; }
	// A dialog opened while the LoadingScreen is on top (App's HOME menu) must never capture `AppEntry` pointers or
	// references: the catalog may be replaced underneath it, and the handover to Home waits for the dialog.
	void showDialog(Dialog d);
	void quitPrompt(); // "Exit OpenShop?" confirmation

	// The top bar every browsing screen draws instead of calling drawTopBar directly: fills in which action
	// buttons are visible (none by default; `topbar=all` previews all three), draws, and dispatches a click.
	// `hide` keeps a capsule off this screen (e.g. Downloads on the Downloads screen itself).
	void topBar(Ui& ui, std::string_view title, std::string_view subtitle = {},
	            std::initializer_list<TopBarAction> hide = {});
	// Where the top-bar buttons lead. M3 replaces openDownloads(), M4 openSettings(); until then each shows a
	// "coming soon" message. Refresh goes to refreshCatalog().
	void openDownloads();
	void openSettings();
	// Home's "My apps" pill. M3 replaces this with the My apps screen (makeMyAppsScreen()); until then it says so.
	void openMyApps();

private:
	void drawDialog();
	// Closes the dialog without a button choice: a Progress dialog's onCancel is called; a Message dialog
	// (including a finished Progress dialog showing its "done" message) is dropped without calling onChoose.
	void dropDialog();
	void openDevStartScreen();
	void autoRefreshTick();
	void startIconPrune();
	void comingSoon(const char* what);

	Catalog catalog_;
	install::AppsDeviceSetting appsDevice_;
	net::Worker worker_{net::defaultTransportFactory()};
	// Declared after the worker: its jobs only hold shared state, but it is destroyed (textures freed) first anyway.
	std::unique_ptr<IconCache> icons_;
	bool iconsPruned_ = false; // the startup prune of icons of apps that left the catalog was started
	// Dev-only (nettest= in devstart); null normally. Declared after the worker so it is destroyed first.
	std::unique_ptr<net::NetSelfTest> netTest_;
	// Which LoadingScreen may install, the load token, the generation and the last network fetch (host-tested).
	LoadTracker loads;
	uint32_t homeSinceMs_ = 0; // autorefresh dev hook: when Home became the only screen (0 = not now)
	std::vector<std::unique_ptr<Screen>> stack;
	std::unique_ptr<Screen> pendingPush;
	std::unique_ptr<Screen> pendingReset;
	bool resetOpensDevStart = false;
	bool pendingPop = false;
	float fade = 1.0f; // 1 = fully white
	int fadeDir = -1;  // +1 fading out to white, -1 fading in
	std::optional<Dialog> dialog;
	float dialogAnim = 0;
	bool quitRequested = false;
};

// Sort orders shared by the screens that open lists.
inline const std::vector<SortMode> kAllSorts = {SortMode::Popular, SortMode::Newest, SortMode::Name};
inline const std::vector<SortMode> kSearchSorts = {SortMode::Relevance, SortMode::Popular, SortMode::Newest};

// Screens
// `opts.mode` selects the startup look (error screen with Retry / Exit) or the Refresh look ("Press B to cancel").
std::unique_ptr<Screen> makeLoadingScreen(const LoaderOptions& opts);
// A failed Refresh: Home shows "Couldn't refresh" on its first frame after the fade-in. Copied strings only.
struct RefreshNotice {
	std::string reason;    // describe()-style reason
	std::string fetchedAt; // formatted date of the catalog that stays in use
};
std::unique_ptr<Screen> makeHomeScreen(std::optional<RefreshNotice> notice = std::nullopt);
std::unique_ptr<Screen> makeListScreen(std::string title, std::vector<const AppEntry*> apps,
                                       std::vector<SortMode> sorts, SortMode initial,
                                       std::string emptyMessage = "No apps here yet.");
std::unique_ptr<Screen> makeDetailScreen(const AppEntry& app);
std::unique_ptr<Screen> makeSearchScreen();
#ifdef OPENSHOP_DEBUGTOOLS
// Debug builds only: the on-screen network test bench (screens/NetTestScreen.cpp). Opens at startup; Minus on Home.
std::unique_ptr<Screen> makeNetTestScreen();
#endif
