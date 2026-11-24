#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/Catalog.h"
#include "gfx/Gfx.h"
#include "gfx/Text.h"
#include "ui/Ui.h"

class App;

class Screen {
public:
	virtual ~Screen() = default;
	virtual void draw(App& app, Ui& ui) = 0;
};

struct Dialog {
	enum class Kind { Message, Progress };
	Kind kind = Kind::Message;
	std::string title, message;
	std::vector<std::string> buttons{"OK"};
	int cancelIndex = 0;                 // chosen by B / Back
	int primaryIndex = -1;               // drawn as the blue button
	std::function<void(int)> onChoose;   // index of the chosen button
	// Progress dialogs (preview only for now): message shown when finished.
	std::string doneTitle, doneMessage;
	float progress = 0;
};

class App {
public:
	Gfx gfx;
	Text text;
	Ui ui{gfx, text};
	Catalog catalog;

	bool init(int argc, char** argv);
	void run();
	void shutdown();

	void push(std::unique_ptr<Screen> s);
	void pop();
	void quit() { quitRequested = true; }
	void showDialog(Dialog d);
	void quitPrompt(); // "Exit OpenShop?" confirmation

private:
	void drawDialog();
	void openDevStartScreen();

	std::vector<std::unique_ptr<Screen>> stack;
	std::unique_ptr<Screen> pendingPush;
	bool pendingPop = false;
	float fade = 1.0f; // 1 = fully white
	int fadeDir = -1;  // +1 fading out to white, -1 fading in
	std::optional<Dialog> dialog;
	float dialogAnim = 0;
	bool quitRequested = false;
};

// Screens
std::unique_ptr<Screen> makeHomeScreen();
std::unique_ptr<Screen> makeListScreen(std::string title, std::vector<const AppEntry*> apps,
                                       std::vector<SortMode> sorts, SortMode initial,
                                       std::string emptyMessage = "No apps here yet.");
std::unique_ptr<Screen> makeDetailScreen(const AppEntry& app);
std::unique_ptr<Screen> makeSearchScreen();
