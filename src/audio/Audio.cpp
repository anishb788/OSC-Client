#include "audio/Audio.h"

#include <SDL_mixer.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Log.h"

namespace audio {
namespace {
bool gOpen = false;
Mix_Music* gMusic = nullptr;
bool gMusicOn = true;
bool gSuspended = false;
Mix_Chunk* gSfx[(int)Sfx::Count] = {};

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

// The HOME menu chime (original, not a sample of anything): two soft bell-like notes a fifth apart, the
// second starting 70 ms after the first, each with a couple of quiet overtones and a gentle decay.
std::vector<Sint16> chime(float gain) {
	const float seconds = 0.55f;
	const struct { float freq, start; } notes[] = {{1046.5f, 0.0f}, {1568.0f, 0.07f}}; // C6, G6
	int n = (int)(seconds * kSynthRate);
	std::vector<float> mix(n, 0.0f);
	for (auto& note : notes) {
		int s0 = (int)(note.start * kSynthRate);
		for (int i = s0; i < n; i++) {
			float t = (float)(i - s0) / kSynthRate;
			float env = std::min(1.0f, t / 0.003f) * std::exp(-7.0f * t);
			double w = 2.0 * M_PI * note.freq * t;
			mix[i] += (float)(std::sin(w) * 0.75 + std::sin(w * 2.0) * 0.18 + std::sin(w * 3.0) * 0.07) * env * 0.5f;
		}
	}
	// The decay has not quite reached zero at the end; a 10 ms linear fade-out avoids a click when it stops.
	const int fade = kSynthRate / 100;
	for (int i = 0; i < fade; i++) mix[n - fade + i] *= 1.0f - (float)(i + 1) / fade;
	std::vector<Sint16> out(n);
	for (int i = 0; i < n; i++) out[i] = (Sint16)(std::clamp(mix[i] * gain, -1.0f, 1.0f) * 32767);
	return out;
}

// Applies the combined state: music plays only when the user wants it and nothing suspends it.
void applyMusicState() {
	if (!gMusic) return;
	if (gMusicOn && !gSuspended) Mix_ResumeMusic(); else Mix_PauseMusic();
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
	gSfx[(int)Sfx::Home] = makeChunk(chime(0.32f));

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
	applyMusicState();
}

void setMusicSuspended(bool suspended) {
	if (suspended == gSuspended) return;
	gSuspended = suspended;
	if (gMusic) {
		if (suspended) LOG("Music paused (menu open)");
		else LOG("Menu closed: music %s", gMusicOn ? "resumed" : "stays off (switched off by the user)");
	}
	applyMusicState();
}
} // namespace audio
