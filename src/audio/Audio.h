#pragma once
#include <string>

namespace audio {
enum class Sfx { Hover, Select, Back, Page, Error, Home, Count };

// Opens the mixer, synthesizes UI sounds and starts background music if the
// user supplied bgm.mp3 (or bgm.ogg) in dataDir.
void init(const std::string& dataDir);
void shutdown();
void play(Sfx s);

bool hasMusic();
bool musicEnabled();  // the user's Music on/off choice (unaffected by suspension)
void toggleMusic();
// Temporarily silences the music (e.g. while the HOME exit menu is open) without changing the user's
// on/off choice. Cheap to call every frame; only acts when the value changes.
void setMusicSuspended(bool suspended);
} // namespace audio
