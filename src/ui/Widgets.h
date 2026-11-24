#pragma once
#include <string_view>

#include "gfx/Gfx.h"
#include "ui/Ui.h"

struct AppEntry;

enum class Pill { Normal, Primary, Selected };

// White card with soft shadow; hov (0..1) grows it slightly and adds the blue focus ring.
void drawCard(Ui& ui, Rect r, float hov, float radius = 14);

bool pillButton(Ui& ui, Id id, Rect r, std::string_view label, Pill style = Pill::Normal,
                const Icon* icon = nullptr, int fontSize = 18);
bool arrowButton(Ui& ui, Id id, float cx, float cy, Icon dir, float radius = 20);

// Placeholder app icon (real 128x48 PNG icons arrive with the live catalog).
void drawAppIcon(Ui& ui, const AppEntry& app, Rect r);

// Small rounded label; returns its width.
float drawChip(Ui& ui, float x, float y, std::string_view label, Color c, const Icon* icon = nullptr);

void drawTopBar(Ui& ui, std::string_view title, std::string_view subtitle = {});
void drawBottomBar(Ui& ui);
// Standard "Back" pill in the bottom-left; true if clicked or B pressed.
bool backButton(Ui& ui, std::string_view label = "Back");

Color categoryColor(std::string_view category);
