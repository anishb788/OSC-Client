#include "platform/Platform.h"

#include <SDL.h>

#include "core/Log.h"

#ifdef __wii__
#include <ogc/conf.h>
#include <wiiuse/wpad.h>
#endif

namespace platform {
namespace {
std::string gDataDir;
uint32_t gPrevHeld = 0;
float gRepeatTimer = 0;
float gLastPx = -1, gLastPy = -1;

constexpr float kRepeatDelay = 0.38f;
constexpr float kRepeatRate = 0.085f;

// Turns raw held state into pressed / auto-repeat edges. `downEvents` are buttons
// that went down during this poll even if already released again (quick clicks).
void finishFrame(Input& in, uint32_t held, float dt, uint32_t downEvents = 0) {
	in.held = held;
	in.pressed = (held & ~gPrevHeld) | downEvents;
	in.repeat = in.pressed;
	uint32_t dirs = held & BTN_DIRS;
	if (in.pressed & BTN_DIRS) {
		gRepeatTimer = kRepeatDelay;
	} else if (dirs) {
		gRepeatTimer -= dt;
		if (gRepeatTimer <= 0) {
			in.repeat |= dirs;
			gRepeatTimer += kRepeatRate;
		}
	}
	gPrevHeld = held;

	in.pointerMoved = in.pointerValid && (in.px != gLastPx || in.py != gLastPy);
	gLastPx = in.px;
	gLastPy = in.py;
}
} // namespace

const std::string& dataDir() { return gDataDir; }

#ifdef __wii__

void init(int argc, char** argv) {
	// HBC passes the full path of boot.dol, e.g. "sd:/apps/OpenShop/boot.dol".
	if (argc > 0 && argv[0]) {
		std::string p = argv[0];
		size_t slash = p.rfind('/');
		if (p.find(":/") != std::string::npos && slash != std::string::npos) gDataDir = p.substr(0, slash);
	}
	if (gDataDir.empty()) gDataDir = "sd:/apps/OpenShop";
	LOG("Data dir: %s (argc=%d)", gDataDir.c_str(), argc);
}

bool widescreen() { return CONF_GetAspectRatio() == CONF_ASPECT_16_9; }

const char* exitLabel() { return "Exit"; }

void poll(Input& in, float dt, int layoutW) {
	in = Input{};
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT: in.quit = true; break;
		case SDL_TEXTINPUT: in.text += e.text.text; break;
		case SDL_KEYDOWN:
			if (e.key.keysym.sym == SDLK_BACKSPACE) in.backspaces++;
			if (e.key.keysym.sym == SDLK_RETURN) in.enterKey = true;
			break;
		}
	}

	// SDL already pumps WPAD while processing events; read the result directly so
	// A/B keep their Wii meaning (SDL maps B to left click).
	WPADData* d = WPAD_Data(0);
	uint32_t h = d ? d->btns_h : 0;
	uint32_t held = in.enterKey ? (uint32_t)BTN_CONFIRM : 0u;
	auto map = [&](uint32_t wpad, uint32_t btn) { if (h & wpad) held |= btn; };
	map(WPAD_BUTTON_A, BTN_CONFIRM);
	map(WPAD_BUTTON_B, BTN_BACK);
	map(WPAD_BUTTON_UP, BTN_UP);
	map(WPAD_BUTTON_DOWN, BTN_DOWN);
	map(WPAD_BUTTON_LEFT, BTN_LEFT);
	map(WPAD_BUTTON_RIGHT, BTN_RIGHT);
	map(WPAD_BUTTON_PLUS, BTN_PLUS);
	map(WPAD_BUTTON_MINUS, BTN_MINUS);
	map(WPAD_BUTTON_HOME, BTN_HOME);
	map(WPAD_BUTTON_1, BTN_ONE);
	map(WPAD_CLASSIC_BUTTON_A, BTN_CONFIRM);
	map(WPAD_CLASSIC_BUTTON_B, BTN_BACK);
	map(WPAD_CLASSIC_BUTTON_UP, BTN_UP);
	map(WPAD_CLASSIC_BUTTON_DOWN, BTN_DOWN);
	map(WPAD_CLASSIC_BUTTON_LEFT, BTN_LEFT);
	map(WPAD_CLASSIC_BUTTON_RIGHT, BTN_RIGHT);
	map(WPAD_CLASSIC_BUTTON_PLUS, BTN_PLUS);
	map(WPAD_CLASSIC_BUTTON_MINUS, BTN_MINUS);
	map(WPAD_CLASSIC_BUTTON_HOME, BTN_HOME);
	map(WPAD_CLASSIC_BUTTON_FULL_R, BTN_PLUS);
	map(WPAD_CLASSIC_BUTTON_FULL_L, BTN_MINUS);

	if (d && d->ir.valid) {
		in.pointerValid = true;
		in.px = d->ir.x * layoutW / 640.0f;
		in.py = d->ir.y;
	}
	finishFrame(in, held, dt);
}

#else // PC

namespace {
uint32_t gKeysHeld = 0, gMouseHeld = 0;
bool gMouseInside = false;
float gMx = -1, gMy = -1;

uint32_t keyToBtn(SDL_Keycode k) {
	switch (k) {
	case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_CONFIRM;
	case SDLK_ESCAPE: return BTN_BACK;
	case SDLK_UP: return BTN_UP;
	case SDLK_DOWN: return BTN_DOWN;
	case SDLK_LEFT: return BTN_LEFT;
	case SDLK_RIGHT: return BTN_RIGHT;
	case SDLK_PAGEDOWN: return BTN_PLUS;
	case SDLK_PAGEUP: return BTN_MINUS;
	case SDLK_F1: return BTN_HOME;
	case SDLK_F2: return BTN_ONE;
	default: return 0;
	}
}
} // namespace

void init(int argc, char** argv) {
	const char* env = SDL_getenv("OPENSHOP_DATA");
	gDataDir = env ? env : "sdcard/apps/OpenShop";
	(void)argc; (void)argv;
	LOG("Data dir: %s", gDataDir.c_str());
}

bool widescreen() {
	const char* env = SDL_getenv("OPENSHOP_WIDE");
	return env && env[0] == '1';
}

const char* exitLabel() { return "Quit"; }

void poll(Input& in, float dt, int layoutW) {
	(void)layoutW;
	in = Input{};
	uint32_t downs = 0;
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT: in.quit = true; break;
		case SDL_TEXTINPUT: in.text += e.text.text; break;
		case SDL_KEYDOWN:
			if (e.key.keysym.sym == SDLK_BACKSPACE) in.backspaces++;
			if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) in.enterKey = true;
			if (!e.key.repeat) {
				gKeysHeld |= keyToBtn(e.key.keysym.sym);
				downs |= keyToBtn(e.key.keysym.sym);
			}
			break;
		case SDL_KEYUP: gKeysHeld &= ~keyToBtn(e.key.keysym.sym); break;
		case SDL_MOUSEMOTION:
			gMx = (float)e.motion.x; gMy = (float)e.motion.y;
			gMouseInside = true;
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			uint32_t b = e.button.button == SDL_BUTTON_LEFT ? (uint32_t)BTN_CONFIRM
			           : e.button.button == SDL_BUTTON_RIGHT ? (uint32_t)BTN_BACK : 0u;
			gMx = (float)e.button.x; gMy = (float)e.button.y;
			if (e.type == SDL_MOUSEBUTTONDOWN) { gMouseHeld |= b; downs |= b; }
			else gMouseHeld &= ~b;
			break;
		}
		case SDL_MOUSEWHEEL: in.wheel += e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0; break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_LEAVE) gMouseInside = false;
			if (e.window.event == SDL_WINDOWEVENT_ENTER) gMouseInside = true;
			if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) gKeysHeld = gMouseHeld = 0;
			break;
		}
	}
	in.pointerValid = gMouseInside && gMx >= 0;
	in.px = gMx;
	in.py = gMy;
	finishFrame(in, gKeysHeld | gMouseHeld, dt, downs);
}

#endif

} // namespace platform
