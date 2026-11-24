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

	// The next item() call does not play the Select click when activated (for items whose action plays its own
	// sound, e.g. the Exit button opening the HOME menu with its chime). Applies to exactly one item() call.
	void silenceNextActivation() { silenceNext = true; }

	// Registers an interactive rect. Returns true when activated this frame.
	bool item(Id id, Rect r);
	// Eased 0..1 focus amount for drawing hover effects.
	float hover(Id id) const;
	bool focused(Id id) const { return focus == id && focus != 0; }
	void setFocus(Id id) { focus = id; }
	// Gives `id` focus in D-pad mode so A works at once, but only when this layer is live (no dialog or fade on
	// top), nothing is focused yet and no pointer is aimed at the screen. Safe to call every frame. The pointer
	// anchor is moved far away so the pointer's first movement always takes focus back.
	void focusForDpad(Id id) {
		if (!onTop() || focus != 0 || in.pointerValid) return;
		usePointer = false;
		anchorX = anchorY = -1e4f;
		focus = id;
	}
	// Focus target when the D-pad is first used on a screen.
	void setDefaultFocus(Id id) { defaultFocus = id; }

	// A direction was pressed but no item lies that way (screens use it for paging).
	// Raw input queries only answer for the topmost (modal) layer.
	uint32_t unhandledNav() const { return onTop() ? navUnhandled : 0; }
	bool pressed(uint32_t b) const { return onTop() && (in.pressed & b); }
	int wheel() const { return onTop() ? in.wheel : 0; }
	bool onTop() const { return inputEnabled && layer == activeLayer; }
	bool pointerMode() const { return usePointer; }

	// Modality is explicit: App sets the modal depth (0 = no dialog, 1 = a dialog is open, whatever its kind)
	// before beginFrame(). Only items and raw input on that layer are live; the screen underneath (layer 0)
	// is blocked even when the dialog registers no items (e.g. a Progress dialog). A change of depth resets focus.
	void setModalDepth(int d) { modal = d; }
	// Draw-side layer bookkeeping: a dialog wraps its widgets in pushLayer()/popLayer().
	void pushLayer() { layer++; }
	void popLayer() { layer--; }
	// Prevents any item from being activated by Confirm this frame.
	void consumeConfirm() { activationUsed = true; }
	// Forgets focus, the hover-sound memory and the D-pad default (e.g. when one dialog replaces another).
	void clearFocus() { focus = 0; lastFocus = 0; defaultFocus = 0; }
	void resetFocusMemory() { clearFocus(); items.clear(); prevItems.clear(); }

private:
	struct Item { Id id; Rect r; int layer; };
	const Item* findPrev(Id id) const;

	std::vector<Item> items, prevItems;
	std::unordered_map<Id, float> anim;
	Id focus = 0, defaultFocus = 0, lastFocus = 0;
	bool usePointer = true;
	float anchorX = 0, anchorY = 0;
	bool activationUsed = false;
	bool silenceNext = false;
	uint32_t navUnhandled = 0;
	int layer = 0, activeLayer = 0, modal = 0;
};
