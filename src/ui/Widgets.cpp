#include "ui/Widgets.h"

#include "catalog/Catalog.h"
#include "gfx/Text.h"
#include "gfx/Theme.h"

void drawCard(Ui& ui, Rect r, float hov, float radius) {
	Rect g = r.inset(-2.5f * hov);
	ui.g.softShadow(g, radius, 1.0f + hov * 0.8f);
	if (hov > 0.01f) ui.g.fillRoundRect(g.inset(-3), radius + 3, theme::accent.withAlpha(0.28f * hov));
	float bw = 1.5f + 1.5f * hov;
	ui.g.roundRectBorder(g, radius, bw, lerp(theme::border, theme::accent, hov), theme::card);
}

bool pillButton(Ui& ui, Id id, Rect r, std::string_view label, Pill style, const Icon* icon, int fontSize) {
	bool clicked = ui.item(id, r);
	float hov = ui.hover(id);
	Rect g = r.inset(-2.0f * hov);
	float rad = g.h / 2;

	Color fill = theme::card, edge = lerp(theme::border, theme::accent, hov), fg = theme::text;
	if (style == Pill::Primary) {
		fill = lerp(theme::accent, Color{90, 205, 245}, hov);
		edge = lerp(theme::accentDark, Color{255, 255, 255}, hov * 0.6f);
		fg = theme::white;
	} else if (style == Pill::Selected) {
		fill = theme::accent;
		edge = lerp(theme::accentDark, theme::accent, hov);
		fg = theme::white;
	}

	ui.g.softShadow(g, rad, 0.8f + hov * 0.6f);
	if (hov > 0.01f) ui.g.fillRoundRect(g.inset(-3), rad + 3, theme::accent.withAlpha(0.25f * hov));
	ui.g.roundRectBorder(g, rad, 1.5f + 1.5f * hov, edge, fill);
	// Glossy top highlight.
	ui.g.fillRoundRect({g.x + rad * 0.6f, g.y + 3, g.w - rad * 1.2f, g.h * 0.34f}, g.h * 0.17f,
	                   Color{255, 255, 255, (Uint8)(style == Pill::Normal ? 0 : 50)});

	int tw = ui.t.width(Font::Bold, fontSize, label);
	float iconSize = icon ? fontSize * 1.15f : 0;
	float gap = icon && !label.empty() ? 8 : 0;
	float total = std::min(iconSize + gap + tw, g.w - 24);
	float x = g.cx() - total / 2;
	if (icon) {
		ui.g.icon(*icon, {x, g.cy() - iconSize / 2, iconSize, iconSize}, fg);
		x += iconSize + gap;
	}
	ui.t.drawIn(Font::Bold, fontSize, label, {x, g.y, total - iconSize - gap + 1, g.h}, fg);
	return clicked;
}

bool arrowButton(Ui& ui, Id id, float cx, float cy, Icon dir, float radius) {
	Rect r{cx - radius, cy - radius, radius * 2, radius * 2};
	bool clicked = ui.item(id, r);
	float hov = ui.hover(id);
	float rr = radius + 2 * hov;
	ui.g.softShadow({cx - rr, cy - rr, rr * 2, rr * 2}, rr, 0.8f);
	ui.g.circle(cx, cy, rr, lerp(theme::border, theme::accent, hov));
	ui.g.circle(cx, cy, rr - 2 - hov, lerp(theme::card, theme::accent, hov));
	ui.g.icon(dir, {cx - rr * 0.6f, cy - rr * 0.6f, rr * 1.2f, rr * 1.2f}, lerp(theme::accent, theme::white, hov));
	return clicked;
}

Color categoryColor(std::string_view c) {
	if (c == "games") return {255, 146, 64};
	if (c == "emulators") return {150, 112, 232};
	if (c == "utilities") return {52, 180, 232};
	if (c == "media") return {238, 92, 146};
	if (c == "demos") return {84, 192, 112};
	return {128, 140, 152};
}

void drawAppIcon(Ui& ui, const AppEntry& app, Rect r) {
	Color base = categoryColor(app.category);
	// Vary the hue a little per app so neighbouring placeholders differ.
	uint32_t h = makeId(app.slug);
	Color top = lerp(base, theme::white, 0.25f + (h % 7) * 0.03f);
	Color bottom = lerp(base, Color{0, 0, 0}, 0.08f + (h % 5) * 0.03f);
	float rad = std::min(r.h * 0.22f, 10.0f);
	ui.g.fillRoundRect(r, rad, bottom);
	ui.g.pushClip(r.inset(1));
	ui.g.gradientV(r.inset(1), top, bottom);
	ui.g.popClip();
	ui.g.fillRoundRect({r.x + 3, r.y + 2, r.w - 6, r.h * 0.4f}, rad * 0.8f, Color{255, 255, 255, 40});
	int size = std::max(10, (int)(r.h * 0.36f));
	ui.t.drawIn(Font::Bold, size, app.name, r.inset(r.h * 0.18f, 0), theme::white, Align::Center);
}

float drawChip(Ui& ui, float x, float y, std::string_view label, Color c, const Icon* icon) {
	const int fs = 13;
	float h = 22, iconW = icon ? 14 : 0, pad = 9;
	float w = pad * 2 + iconW + (icon ? 5 : 0) + ui.t.width(Font::Bold, fs, label);
	ui.g.fillRoundRect({x, y, w, h}, h / 2, c.withAlpha(0.16f));
	float tx = x + pad;
	if (icon) { ui.g.icon(*icon, {tx, y + 4, iconW, iconW}, c); tx += iconW + 5; }
	ui.t.draw(Font::Bold, fs, label, tx, y + h / 2 - ui.t.lineHeight(Font::Bold, fs) / 2.0f, lerp(c, Color{0, 0, 0}, 0.25f));
	return w;
}

void drawTopBar(Ui& ui, std::string_view title, std::string_view subtitle) {
	float W = (float)ui.g.width();
	const float h = theme::topBarH;
	ui.g.gradientV({0, h, W, 10}, Color{0, 40, 80, 26}, Color{0, 40, 80, 0});
	ui.g.gradientV({0, 0, W, h}, theme::white, Color{246, 249, 251});
	ui.g.fillRect({0, h - 3, W, 3}, theme::accent);
	ui.g.fillRect({0, h - 3, W, 1}, Color{140, 220, 248});

	float x = theme::margin;
	std::string t = ui.t.ellipsize(Font::Bold, 24, title, (int)(W * 0.62f));
	x += ui.t.draw(Font::Bold, 24, t, x, h / 2 - ui.t.lineHeight(Font::Bold, 24) / 2.0f - 1, theme::text);
	if (!subtitle.empty()) {
		x += 14;
		ui.t.drawIn(Font::Medium, 15, subtitle, {x, 0, W - x - theme::margin, h - 2}, theme::textMuted);
	}
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
	static const Icon chevron = Icon::ChevronLeft;
	Rect r{theme::margin, H - theme::bottomBarH + 12, 132, 40};
	bool clicked = pillButton(ui, makeId("bottom:back"), r, label, Pill::Normal, &chevron, 17);
	return clicked || ui.pressed(BTN_BACK);
}
