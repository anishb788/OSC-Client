#pragma once
#include <string>

#include "core/Input.h"

namespace platform {
// Call after SDL_Init.
void init(int argc, char** argv);
// Pumps SDL events and fills `in` for this frame. layoutW is the logical width.
void poll(Input& in, float dt, int layoutW);
bool widescreen();
// Directory holding user files (bgm.mp3, cache, settings).
// Wii: the app's folder on SD/USB, e.g. sd:/apps/OpenShop. PC: ./sdcard/apps/OpenShop.
const std::string& dataDir();
const char* exitLabel();
// The music key as the hint on Home names it: "Press 1 to toggle music" (Wii) / "Press F2 to toggle music" (PC).
const char* musicToggleHint();
// Writes `data` to `path` atomically: "<path>.tmp" first, then it replaces the target (libfat cannot rename over an
// existing file, so the old one is parked as .bak meanwhile). Logs failures; returns false on error.
bool writeFile(const std::string& path, const std::string& data);
// Reads a whole file into `out` (one fread). Returns false, with `out` empty, if it is missing, unreadable or larger
// than `maxBytes` (the default suits small text files; callers reading bigger data pass their own bound).
bool readFile(const std::string& path, std::string& out, size_t maxBytes = 1u << 20);
} // namespace platform
