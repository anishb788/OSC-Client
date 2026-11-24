#pragma once
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Input.h"
#include "core/Types.h"

class Gfx;
class Text;

using Id = uint32_t;
Id makeId(std::string_view s, uint32_t salt = 0);

// Immediate-mode UI state. Widgets call item() every frame with a stable id;
// Ui decides focus from the pointer (IR / mouse) or from D-pad spatial
// navigation using the rects registered on the previous frame.
class Ui {
public:
	Gfx& g;
	Text& t;
	Input in;
	float dt = 0;
	double time = 0;
	bool inputEnabled = true;

	Ui(Gfx& gfx, Text& text) : g(gfx), t(text) {}

	void beginFrame(const Input& input, float dt);
	void endFrame();

	// Registers an interactive rect. Returns true when activated this frame.
	bool item(Id id, Rect r);
	// Eased 0..1 focus amount for drawing hover effects.
	float hover(Id id) const;
	bool focused(Id id) const { return focus == id && focus != 0; }
	void setFocus(Id id) { focus = id; }
	// Focus target when the D-pad is first used on a screen.
	void setDefaultFocus(Id id) { defaultFocus = id; }

	// A direction was pressed but no item lies that way (screens use it for paging).
	// Raw input queries only answer for the topmost (modal) layer.
	uint32_t unhandledNav() const { return onTop() ? navUnhandled : 0; }
	bool pressed(uint32_t b) const { return onTop() && (in.pressed & b); }
	int wheel() const { return onTop() ? in.wheel : 0; }
	bool onTop() const { return inputEnabled && layer == activeLayer; }
	bool pointerMode() const { return usePointer; }

	// Items registered while a higher layer is active are not interactive (modals).
	void pushLayer() { layer++; }
	void popLayer() { layer--; }
	// Prevents any item from being activated by Confirm this frame.
	void consumeConfirm() { activationUsed = true; }
	void resetFocusMemory() { focus = 0; defaultFocus = 0; items.clear(); prevItems.clear(); }

private:
	struct Item { Id id; Rect r; int layer; };
	const Item* findPrev(Id id) const;

	std::vector<Item> items, prevItems;
	std::unordered_map<Id, float> anim;
	Id focus = 0, defaultFocus = 0, lastFocus = 0;
	bool usePointer = true;
	float anchorX = 0, anchorY = 0;
	bool activationUsed = false;
	uint32_t navUnhandled = 0;
	int layer = 0, activeLayer = 0;
};
