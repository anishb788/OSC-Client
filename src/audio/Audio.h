#pragma once
#include <string>

namespace audio {
enum class Sfx { Hover, Select, Back, Page, Error };

// Opens the mixer, synthesizes UI sounds and starts background music if the
// user supplied bgm.mp3 (or bgm.ogg) in dataDir.
void init(const std::string& dataDir);
void shutdown();
void play(Sfx s);

bool hasMusic();
bool musicEnabled();
void toggleMusic();
} // namespace audio
