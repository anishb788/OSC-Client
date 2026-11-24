#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/Catalog.h"
#include "catalog/CatalogLoader.h"
#include "core/DevOptions.h"
#include "gfx/Gfx.h"
#include "gfx/Text.h"
#include "ui/Ui.h"

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
	// INVARIANT: it is installed exactly once, while only the LoadingScreen exists, and never replaced
	// or modified afterwards. Screens rely on that storage staying put for the whole session:
	// DetailScreen holds a `const AppEntry&`, ListScreen and SearchScreen hold `const AppEntry*` lists
	// (HomeScreen only borrows `featured()` within a frame). Replacing `Catalog::apps` while they exist
	// would leave them dangling. `catalog` is a const reference so the compiler enforces the read-only part.
	//
	// Future in-session refresh: either make every holder store a slug and re-resolve with
	// `catalog.find(slug)` each draw (pop if it is gone), or hand out `std::shared_ptr<const Catalog>`
	// snapshots so old screens keep the catalog they were built from alive. See the epic.
	const Catalog& catalog{catalog_};
	DevOptions opts; // parsed once in init()

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
	// The only way to set the catalog. Refuses (LOG, returns false) if it was already installed, if any screen
	// other than the LoadingScreen exists, or if a screen change is pending (see catalogInstallRefusal).
	[[nodiscard]] bool installCatalog(Catalog&& c);
	void quit() { quitRequested = true; }
	void showDialog(Dialog d);
	void quitPrompt(); // "Exit OpenShop?" confirmation

private:
	void drawDialog();
	// Closes the dialog without a button choice: a Progress dialog's onCancel is called; a Message dialog
	// (including a finished Progress dialog showing its "done" message) is dropped without calling onChoose.
	void dropDialog();
	void openDevStartScreen();

	Catalog catalog_;
	bool catalogInstalled = false;
	const Screen* loadingScreen = nullptr; // the startup screen, the only one allowed to exist at install time
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
std::unique_ptr<Screen> makeLoadingScreen(const LoaderOptions& opts);
std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeListScreen(std::string title, std::vector<const AppEntry*> apps,
                                       std::vector<SortMode> sorts, SortMode initial,
                                       std::string emptyMessage = "No apps here yet.");
std::unique_ptr<Screen> makeDetailScreen(const AppEntry& app);
std::unique_ptr<Screen> makeSearchScreen();
