#include "audio/Audio.h"

#include <SDL_mixer.h>

#include <cmath>
#include <vector>

#include "core/Log.h"

namespace audio {
namespace {
bool gOpen = false;
Mix_Music* gMusic = nullptr;
bool gMusicOn = true;
Mix_Chunk* gSfx[5] = {};

constexpr int kSynthRate = 48000;
constexpr int kMusicVolume = MIX_MAX_VOLUME * 55 / 100;

// A short sine "blip" sweeping f0 -> f1 with a fast attack and exponential decay.
std::vector<Sint16> blip(float f0, float f1, float seconds, float gain) {
	int n = (int)(seconds * kSynthRate);
	std::vector<Sint16> out(n);
	double phase = 0;
	for (int i = 0; i < n; i++) {
		float t = (float)i / n;
		float f = f0 + (f1 - f0) * t;
		phase += 2.0 * M_PI * f / kSynthRate;
		float env = std::min(1.0f, i / (0.004f * kSynthRate)) * std::exp(-5.0f * t);
		float s = (float)std::sin(phase) * 0.8f + (float)std::sin(phase * 2) * 0.2f;
		out[i] = (Sint16)(s * env * gain * 32767);
	}
	return out;
}

// Converts mono S16 at kSynthRate into the mixer's format.
Mix_Chunk* makeChunk(const std::vector<Sint16>& pcm) {
	int freq = 0, channels = 0;
	Uint16 format = 0;
	Mix_QuerySpec(&freq, &format, &channels);
	SDL_AudioCVT cvt;
	if (SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, 1, kSynthRate, format, (Uint8)channels, freq) < 0) return nullptr;
	cvt.len = (int)(pcm.size() * sizeof(Sint16));
	cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult);
	SDL_memcpy(cvt.buf, pcm.data(), cvt.len);
	if (cvt.needed) SDL_ConvertAudio(&cvt);
	Mix_Chunk* c = Mix_QuickLoad_RAW(cvt.buf, cvt.needed ? cvt.len_cvt : cvt.len);
	if (c) c->allocated = 1; // let Mix_FreeChunk free the buffer
	return c;
}
} // namespace

void init(const std::string& dataDir) {
	Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG);
	if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
		LOG("Mix_OpenAudio: %s", Mix_GetError());
		return;
	}
	gOpen = true;
	Mix_AllocateChannels(8);

	gSfx[(int)Sfx::Hover] = makeChunk(blip(1900, 2100, 0.035f, 0.10f));
	gSfx[(int)Sfx::Select] = makeChunk(blip(880, 1320, 0.09f, 0.22f));
	gSfx[(int)Sfx::Back] = makeChunk(blip(1100, 700, 0.09f, 0.20f));
	gSfx[(int)Sfx::Page] = makeChunk(blip(1250, 1500, 0.06f, 0.14f));
	gSfx[(int)Sfx::Error] = makeChunk(blip(320, 260, 0.16f, 0.25f));

	for (const char* name : {"bgm.mp3", "bgm.ogg"}) {
		std::string path = dataDir + "/" + name;
		gMusic = Mix_LoadMUS(path.c_str());
		if (gMusic) {
			LOG("Background music: %s", path.c_str());
			Mix_VolumeMusic(kMusicVolume);
			Mix_PlayMusic(gMusic, -1);
			break;
		}
	}
	if (!gMusic) LOG("No background music (put bgm.mp3 in %s)", dataDir.c_str());
}

void shutdown() {
	if (!gOpen) return;
	Mix_HaltMusic();
	if (gMusic) Mix_FreeMusic(gMusic);
	for (auto*& c : gSfx) { if (c) Mix_FreeChunk(c); c = nullptr; }
	Mix_CloseAudio();
	Mix_Quit();
	gOpen = false;
}

void play(Sfx s) {
	if (Mix_Chunk* c = gOpen ? gSfx[(int)s] : nullptr) Mix_PlayChannel(-1, c, 0);
}

bool hasMusic() { return gMusic != nullptr; }
bool musicEnabled() { return gMusic && gMusicOn; }

void toggleMusic() {
	if (!gMusic) return;
	gMusicOn = !gMusicOn;
	if (gMusicOn) Mix_ResumeMusic(); else Mix_PauseMusic();
}
} // namespace audio
