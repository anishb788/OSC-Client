#pragma once
#include <cstdint>
#include <string>

enum Btn : uint32_t {
	BTN_CONFIRM = 1 << 0, // Wii A        / left click, Enter
	BTN_BACK    = 1 << 1, // Wii B        / right click, Esc
	BTN_UP      = 1 << 2,
	BTN_DOWN    = 1 << 3,
	BTN_LEFT    = 1 << 4,
	BTN_RIGHT   = 1 << 5,
	BTN_PLUS    = 1 << 6, // next page    / PageDown
	BTN_MINUS   = 1 << 7, // prev page    / PageUp
	BTN_HOME    = 1 << 8, // exit menu    / F1
	BTN_ONE     = 1 << 9, // music toggle / F2
	BTN_DIRS    = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT,
};

struct Input {
	float px = -1, py = -1;     // pointer in layout coordinates
	bool pointerValid = false;  // IR pointing at the screen / mouse inside window
	bool pointerMoved = false;
	uint32_t held = 0;
	uint32_t pressed = 0;       // went down this frame
	uint32_t repeat = 0;        // pressed, plus auto-repeat while held (for navigation)
	int wheel = 0;              // +1 up, -1 down
	std::string text;           // typed UTF-8 text (physical keyboards)
	int backspaces = 0;
	bool enterKey = false;      // Return on a physical keyboard
	bool quit = false;
};
