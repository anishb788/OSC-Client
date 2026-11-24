#pragma once
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

struct Color {
	Uint8 r = 255, g = 255, b = 255, a = 255;

	constexpr Color withAlpha(float f) const {
		return {r, g, b, (Uint8)std::clamp(a * f, 0.0f, 255.0f)};
	}
};

inline Color lerp(Color x, Color y, float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	auto m = [t](Uint8 p, Uint8 q) { return (Uint8)(p + (q - p) * t + 0.5f); };
	return {m(x.r, y.r), m(x.g, y.g), m(x.b, y.b), m(x.a, y.a)};
}

inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

struct Rect {
	float x = 0, y = 0, w = 0, h = 0;

	bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
	Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
	Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
	float cx() const { return x + w / 2; }
	float cy() const { return y + h / 2; }
	float right() const { return x + w; }
	float bottom() const { return y + h; }
};

// Frame-rate independent exponential approach toward a target.
inline float approach(float cur, float target, float speed, float dt) {
	float k = 1.0f - std::exp(-speed * dt);
	return cur + (target - cur) * k;
}

inline float easeOutCubic(float t) { t = 1 - std::clamp(t, 0.0f, 1.0f); return 1 - t * t * t; }
