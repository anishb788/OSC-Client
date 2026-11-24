#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "gfx/Gfx.h"
#include "gfx/Theme.h"
#include "ui/TopBarLayout.h"
#include "ui/Ui.h"

struct AppEntry;

enum class Pill { Normal, Primary, Selected };

// White card with soft shadow; hov (0..1) grows it slightly and adds the blue focus ring.
void drawCard(Ui& ui, Rect r, float hov, float radius = 14);

// iconColor overrides the icon's colour (it defaults to the label colour).
bool pillButton(Ui& ui, Id id, Rect r, std::string_view label, Pill style = Pill::Normal,
                std::optional<Icon> icon = std::nullopt, int fontSize = 18,
                std::optional<Color> iconColor = std::nullopt);
bool arrowButton(Ui& ui, Id id, float cx, float cy, Icon dir, float radius = 20);

// Placeholder app icon (real 128x48 PNG icons arrive with the live catalog).
void drawAppIcon(Ui& ui, const AppEntry& app, Rect r);

// Small rounded label; returns its width.
float drawChip(Ui& ui, float x, float y, std::string_view label, Color c, std::optional<Icon> icon = std::nullopt);

// Right-aligned action capsules in the top bar, in on-screen order (see ui/TopBarLayout.h).
enum class TopBarAction { Downloads = 0, Refresh = 1, Settings = 2 };
static_assert(topbar::kCount == 3, "TopBarAction, the icon and id tables in Widgets.cpp and the layout must agree");

// Bubble at a capsule's top-right corner: a count ("2") or a mark ("!"). Drawn whenever it is set.
struct Badge {
	std::string text;
	Color colour = theme::accent;
};

struct TopBarButton {
	bool visible = false;               // hidden buttons collapse: the visible ones stay right-aligned
	bool active = false;                // accent outline (e.g. while a download runs)
	std::optional<float> progress;      // 0..1 strip along the bottom of the capsule
	std::optional<Badge> badge;
};

struct TopBarButtons {
	std::array<TopBarButton, topbar::kCount> b;
	TopBarButton& operator[](TopBarAction a) { return b[(size_t)a]; }
	const TopBarButton& operator[](TopBarAction a) const { return b[(size_t)a]; }
};

// Draws the bar. Returns the action activated this frame, if any (pointer click or A on a focused capsule).
std::optional<TopBarAction> drawTopBar(Ui& ui, std::string_view title, std::string_view subtitle = {},
                                       const TopBarButtons* buttons = nullptr);
// True when one of the top-bar capsules has focus (screens that give Enter another meaning check this first).
bool anyTopBarFocused(const Ui& ui);
void drawBottomBar(Ui& ui);
// Standard "Back" pill in the bottom-left; true if clicked or B pressed.
bool backButton(Ui& ui, std::string_view label = "Back");

Color categoryColor(std::string_view category);
