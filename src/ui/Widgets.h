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

// Gloss presets in Theme.h. Destructive (red) is for confirming a deletion (M3).
enum class Pill { Normal, Primary, Selected, Destructive };

// White card with soft shadow; hov (0..1) grows it slightly and adds the blue focus ring.
void drawCard(Ui& ui, Rect r, float hov, float radius = 14);

// Everything about a pill besides its id, rect and label, as designated initialisers:
//   pillButton(ui, id, r, "Search", {.style = Pill::Primary, .icon = Icon::Search, .fontSize = 16})
struct PillSpec {
	Pill style = Pill::Normal;
	std::optional<Icon> icon = std::nullopt;
	int fontSize = 18;
	std::optional<Color> iconColor = std::nullopt; // defaults to the label colour
	bool leftAlign = false;     // icon and label start at padLeft instead of being centred
	float padLeft = 18;
	float contentDy = 0;        // shifts the icon and label vertically
	float radius = -1;          // corner radius; < 0 = a full capsule (keyboard keys use a rounded rectangle)
	std::string_view badge = {}; // right-aligned orange badge ("2 updates"); drawn only when not empty
};

bool pillButton(Ui& ui, Id id, Rect r, std::string_view label, const PillSpec& spec = {});
// Paging / scrolling arrow: a capsule filling `r` (radius = the shorter side / 2) with the chevron centred in it.
// iconFontSize sizes the chevron (about 1.15x). Use rects of at least 36 px on the short side for a pointer target.
bool arrowButton(Ui& ui, Id id, Rect r, Icon dir, int iconFontSize = 18);

// An app's icon: the real PNG (aspect-fit inside `r`, fading in over the placeholder) once the IconCache has it, the
// procedural placeholder (category gradient and name) until then and when the icon is missing or failed. Asking is what
// requests the icon, so call it only for icons that are actually drawn this frame.
void drawAppIcon(Ui& ui, const AppEntry& app, Rect r);

// The width drawChip will draw `label` at (with or without an icon), so a caller can fit it before drawing.
float chipWidth(Ui& ui, std::string_view label, bool hasIcon = false);
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
// Blue pictogram for a category on Home tiles (the grid for "" and for unknown categories).
Icon categoryIcon(std::string_view category);
