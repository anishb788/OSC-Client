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
// Writes `data` to `path` with plain fopen/fwrite/fclose. Logs failures; returns false on error.
bool writeFile(const std::string& path, const std::string& data);
// Reads a whole file into `out`. Returns false (and leaves `out` empty) if it cannot be read.
bool readFile(const std::string& path, std::string& out);
} // namespace platform
