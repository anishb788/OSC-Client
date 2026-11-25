#include "ui/Widgets.h"

#include "catalog/Catalog.h"
#include "gfx/Text.h"
#include "gfx/Theme.h"
#include "ui/TopBarLayout.h"

void drawCard(Ui& ui, Rect r, float hov, float radius) {
	Rect g = r.inset(-2.5f * hov);
	ui.g.softShadow(g, radius, 1.0f + hov * 0.8f);
	if (hov > 0.01f) ui.g.fillRoundRect(g.inset(-3), radius + 3, theme::accent.withAlpha(0.28f * hov));
	float bw = 1.5f + 1.5f * hov;
	ui.g.roundRectBorder(g, radius, bw, lerp(theme::border, theme::accent, hov), theme::card);
}

namespace {
const theme::Gloss& glossFor(Pill style) {
	switch (style) {
	case Pill::Primary: return theme::glossPrimary;
	case Pill::Selected: return theme::glossSelected;
	case Pill::Destructive: return theme::glossDestructive;
	case Pill::Normal: break;
	}
	return theme::glossNormal;
}
} // namespace

// Every button in the UI is drawn here (pills, top-bar capsules, arrows, keyboard keys, dialog buttons), so the
// glossy look lives in one place: Gfx::glossyRoundRect with a preset from Theme.h.
bool pillButton(Ui& ui, Id id, Rect r, std::string_view label, const PillSpec& spec) {
	bool clicked = ui.item(id, r);
	float hov = ui.hover(id);
	Rect g = r.inset(-2.0f * hov);
	float rad = spec.radius >= 0 ? std::min(spec.radius, std::min(g.w, g.h) / 2) : std::min(g.w, g.h) / 2;
	const theme::Gloss& gloss = glossFor(spec.style);
	Color fg = gloss.text;
	const int fontSize = spec.fontSize;
	const std::optional<Icon>& icon = spec.icon;

	ui.g.softShadow(g, rad, 0.8f + hov * 0.6f);
	if (hov > 0.01f) ui.g.fillRoundRect(g.inset(-3), rad + 3, theme::accent.withAlpha(0.25f * hov));
	ui.g.glossyRoundRect(g, rad, gloss, hov);

	// Optional trailing badge ("2 updates"): white bold text on orange, 8 px from the right edge.
	float badgeW = 0;
	if (!spec.badge.empty()) {
		const int bs = 13;
		badgeW = (float)ui.t.width(Font::Bold, bs, spec.badge) + 20;
		Rect b{g.right() - 8 - badgeW, g.cy() - 13 + spec.contentDy, badgeW, 26};
		ui.g.fillRoundRect(b, 13, theme::orange);
		ui.t.drawIn(Font::Bold, bs, spec.badge, b, theme::white, Align::Center);
		badgeW += 8 + 8;
	}

	float dy = spec.contentDy;
	int tw = ui.t.width(Font::Bold, fontSize, label);
	float iconSize = icon ? fontSize * 1.15f : 0;
	float gap = icon && !label.empty() ? (spec.leftAlign ? 10 : 8) : 0;
	float total = std::min(iconSize + gap + tw, g.w - 24 - badgeW);
	float x = spec.leftAlign ? g.x + spec.padLeft : g.cx() - total / 2;
	if (icon) {
		ui.g.icon(*icon, {x, g.cy() - iconSize / 2 + dy, iconSize, iconSize}, spec.iconColor.value_or(fg));
		x += iconSize + gap;
	}
	if (!label.empty()) {
		Rect tr{x, g.y + dy, total - iconSize - gap + 1, g.h};
		if (gloss.textShadow.a) ui.t.drawIn(Font::Bold, fontSize, label, tr.offset(0, 1), gloss.textShadow);
		ui.t.drawIn(Font::Bold, fontSize, label, tr, fg);
	}
	return clicked;
}

bool arrowButton(Ui& ui, Id id, Rect r, Icon dir, int iconFontSize) {
	// A capsule like every other button (same hover growth, focus ring and, from Increment 4e, gloss).
	return pillButton(ui, id, r, {}, {.icon = dir, .fontSize = iconFontSize, .iconColor = theme::accentDark});
}

Color categoryColor(std::string_view c) {
	if (c == "games") return {255, 146, 64};
	if (c == "emulators") return {150, 112, 232};
	if (c == "utilities") return {52, 180, 232};
	if (c == "media") return {238, 92, 146};
	if (c == "demos") return {84, 192, 112};
	return {128, 140, 152};
}

Icon categoryIcon(std::string_view c) {
	if (c == "games") return Icon::Gamepad;
	if (c == "emulators") return Icon::Cartridge;
	if (c == "utilities") return Icon::Wrench;
	if (c == "media") return Icon::Play;
	if (c == "demos") return Icon::Sparkle;
	return Icon::Grid;
}

void drawAppIcon(Ui& ui, const AppEntry& app, Rect r) {
	Color base = categoryColor(app.category);
	// Vary the hue a little per app so neighbouring placeholders differ.
	uint32_t h = makeId(app.slug);
	Color top = lerp(base, theme::white, 0.25f + (h % 7) * 0.03f);
	Color bottom = lerp(base, Color{0, 0, 0}, 0.08f + (h % 5) * 0.03f);
	float rad = std::min(r.h * 0.22f, 10.0f);
	// Anti-aliased base in the bottom colour, the gradient inside it (gradientRoundRect's own edge is not smoothed).
	ui.g.fillRoundRect(r, rad, bottom);
	const Gfx::GradStop stops[2] = {{0, top}, {1, bottom}};
	ui.g.gradientRoundRect(r.inset(1), std::max(0.0f, rad - 1), stops, 2);
	ui.g.fillRoundRect({r.x + 3, r.y + 2, r.w - 6, r.h * 0.4f}, rad * 0.8f, Color{255, 255, 255, 40});
	int size = std::max(10, (int)(r.h * 0.36f));
	ui.t.drawIn(Font::Bold, size, app.name, r.inset(r.h * 0.18f, 0), theme::white, Align::Center);
}

namespace {
constexpr int kChipFont = 13;
constexpr float kChipPad = 9, kChipIconW = 14, kChipIconGap = 5;
} // namespace

float chipWidth(Ui& ui, std::string_view label, bool hasIcon) {
	return kChipPad * 2 + (hasIcon ? kChipIconW + kChipIconGap : 0) + ui.t.width(Font::Bold, kChipFont, label);
}

float drawChip(Ui& ui, float x, float y, std::string_view label, Color c, std::optional<Icon> icon) {
	const int fs = kChipFont;
	float h = 22, iconW = icon ? kChipIconW : 0, pad = kChipPad;
	float w = chipWidth(ui, label, icon.has_value());
	ui.g.fillRoundRect({x, y, w, h}, h / 2, c.withAlpha(0.16f));
	float tx = x + pad;
	if (icon) { ui.g.icon(*icon, {tx, y + 4, iconW, iconW}, c); tx += iconW + 5; }
	ui.t.draw(Font::Bold, fs, label, tx, y + h / 2 - ui.t.lineHeight(Font::Bold, fs) / 2.0f, lerp(c, Color{0, 0, 0}, 0.25f));
	return w;
}

namespace {
constexpr Icon kTopBarIcons[topbar::kCount] = {Icon::Download, Icon::Refresh, Icon::Gear};
constexpr const char* kTopBarIds[topbar::kCount] = {"topbar:downloads", "topbar:refresh", "topbar:settings"};

// One capsule of the top bar: the button itself is a pillButton (so the glossy restyle in Increment 4e reaches
// it through pillButton), plus the capsule-only extras drawn here: active outline and progress strip. The badge
// is drawn separately, after every capsule, so a neighbour's focus glow never covers it.
bool topBarCapsule(Ui& ui, Id id, const topbar::Box& box, Icon icon, const TopBarButton& st) {
	Rect r{box.x, box.y, box.w, box.h};
	if (st.active) ui.g.fillRoundRect(r.inset(-2), r.h / 2 + 2, theme::accent); // peeks out around the pill's border
	// With a progress strip the icon moves up 3 px so the strip has clear space below it.
	bool clicked = pillButton(ui, id, r, {},
	                          {.icon = icon, .fontSize = 16, .iconColor = theme::accentDark,
	                           .contentDy = st.progress ? -3.0f : 0.0f});

	if (st.progress) {
		// 3 px strip 8 px above the bottom edge, between the icon (4.8 px clear above) and the capsule's lower edge.
		Rect track{r.x + 10, r.bottom() - 8 - 3, r.w - 20, 3};
		float p = std::clamp(*st.progress, 0.0f, 1.0f);
		ui.g.fillRoundRect(track, 1.5f, Color{215, 225, 233});
		if (p > 0.01f) ui.g.fillRoundRect({track.x, track.y, std::max(3.0f, track.w * p), track.h}, 1.5f, theme::accent);
	}
	return clicked;
}

void topBarBadge(Ui& ui, const topbar::Box& box, const Badge& badge) {
	if (badge.text.empty()) return;
	float w = std::max(20.0f, (float)ui.t.width(Font::Bold, 12, badge.text) + 12);
	Rect b{box.x + box.w + 6 - w, box.y - 6, w, 20};
	ui.g.roundRectBorder(b, 10, 2, theme::white, badge.colour);
	ui.t.drawIn(Font::Bold, 12, badge.text, b, theme::white, Align::Center);
}
} // namespace

bool anyTopBarFocused(const Ui& ui) {
	for (const char* id : kTopBarIds) if (ui.focused(makeId(id))) return true;
	return false;
}

std::optional<TopBarAction> drawTopBar(Ui& ui, std::string_view title, std::string_view subtitle,
                                       const TopBarButtons* buttons) {
	float W = (float)ui.g.width();
	const float h = theme::topBarH;
	ui.g.gradientV({0, h, W, 10}, Color{0, 40, 80, 26}, Color{0, 40, 80, 0});
	ui.g.gradientV({0, 0, W, h}, theme::white, Color{246, 249, 251});
	ui.g.fillRect({0, h - 3, W, 3}, theme::accent);
	ui.g.fillRect({0, h - 3, W, 1}, Color{140, 220, 248});

	std::array<bool, topbar::kCount> vis{};
	if (buttons) for (int i = 0; i < topbar::kCount; i++) vis[i] = buttons->b[i].visible;
	topbar::Layout lay = topbar::layout(W, theme::margin, vis);

	float x = theme::margin;
	int titleW = ui.t.width(Font::Bold, 24, title);
	int subW = subtitle.empty() ? 0 : ui.t.width(Font::Medium, 15, subtitle);
	topbar::Fit fit = topbar::fit(W, theme::margin, lay.titleRight, (float)titleW, (float)subW);
	std::string t = ui.t.ellipsize(Font::Bold, 24, title, (int)fit.titleMaxW);
	x += ui.t.draw(Font::Bold, 24, t, x, h / 2 - ui.t.lineHeight(Font::Bold, 24) / 2.0f - 1, theme::text);
	if (fit.showSubtitle) {
		x += topbar::kTitleGap;
		ui.t.drawIn(Font::Medium, 15, subtitle, {x, 0, lay.titleRight - x, h - 2}, theme::textMuted);
	}

	std::optional<TopBarAction> activated;
	for (int i = 0; i < topbar::kCount; i++) {
		if (!lay.visible[i]) continue;
		if (topBarCapsule(ui, makeId(kTopBarIds[i]), lay.rect[i], kTopBarIcons[i], buttons->b[i]))
			activated = (TopBarAction)i;
	}
	for (int i = 0; i < topbar::kCount; i++)
		if (lay.visible[i] && buttons->b[i].badge) topBarBadge(ui, lay.rect[i], *buttons->b[i].badge);
	return activated;
}

void drawBottomBar(Ui& ui) {
	float W = (float)ui.g.width(), H = (float)ui.g.height();
	float y = H - theme::bottomBarH;
	ui.g.gradientV({0, y - 8, W, 8}, Color{0, 40, 80, 0}, Color{0, 40, 80, 14});
	ui.g.gradientV({0, y, W, (float)theme::bottomBarH}, Color{241, 245, 248}, theme::bar);
	ui.g.fillRect({0, y, W, 1}, theme::barLine);
	ui.g.fillRect({0, y + 1, W, 1}, theme::white);
}

bool backButton(Ui& ui, std::string_view label) {
	float H = (float)ui.g.height();
	Rect r{theme::margin, H - theme::bottomBarH + 12, 132, 40};
	bool clicked = pillButton(ui, makeId("bottom:back"), r, label, {.icon = Icon::ChevronLeft, .fontSize = 17});
	return clicked || ui.pressed(BTN_BACK);
}
