#include "ui/Ui.h"

#include <cmath>

#include "audio/Audio.h"

Id makeId(std::string_view s, uint32_t salt) {
	uint32_t h = 2166136261u ^ (salt * 16777619u);
	for (char c : s) { h ^= (uint8_t)c; h *= 16777619u; }
	return h ? h : 1;
}

const Ui::Item* Ui::findPrev(Id id) const {
	for (auto& it : prevItems) if (it.id == id && it.layer == activeLayer) return &it;
	return nullptr;
}

void Ui::beginFrame(const Input& input, float frameDt) {
	in = input;
	dt = frameDt;
	time += frameDt;
	navUnhandled = 0;
	activationUsed = false;

	prevItems.swap(items);
	items.clear();
	activeLayer = 0;
	for (auto& it : prevItems) activeLayer = std::max(activeLayer, it.layer);

	if (!inputEnabled) return;

	// Pointer movement takes over focus (after a small dead zone, so IR jitter
	// does not steal focus from the D-pad).
	bool pointerTakesOver = in.pointerMoved &&
		(usePointer || std::hypot(in.px - anchorX, in.py - anchorY) > 8.0f);
	if (pointerTakesOver) {
		usePointer = true;
		focus = 0;
		for (auto it = prevItems.rbegin(); it != prevItems.rend(); ++it) {
			if (it->layer == activeLayer && it->r.contains(in.px, in.py)) { focus = it->id; break; }
		}
	} else if (usePointer && !in.pointerValid && !(in.repeat & BTN_DIRS)) {
		focus = 0;
	}

	// D-pad spatial navigation.
	uint32_t dir = in.repeat & BTN_DIRS;
	if (dir) {
		usePointer = false;
		anchorX = in.px;
		anchorY = in.py;
		const Item* cur = findPrev(focus);
		if (!cur) {
			const Item* def = findPrev(defaultFocus);
			if (!def) {
				for (auto& it : prevItems) if (it.layer == activeLayer) { def = &it; break; }
			}
			if (def) focus = def->id;
		} else {
			float cx = cur->r.cx(), cy = cur->r.cy();
			const Item* best = nullptr;
			float bestScore = 1e9f;
			for (auto& it : prevItems) {
				if (it.layer != activeLayer || it.id == cur->id) continue;
				float dx = it.r.cx() - cx, dy = it.r.cy() - cy;
				float along, across;
				// Candidates must be past the current item's edge in that direction.
				if (dir & BTN_RIGHT) { if (it.r.x < cur->r.right() - 4) continue; along = dx; across = dy; }
				else if (dir & BTN_LEFT) { if (it.r.right() > cur->r.x + 4) continue; along = -dx; across = dy; }
				else if (dir & BTN_DOWN) { if (it.r.y < cur->r.bottom() - 4) continue; along = dy; across = dx; }
				else { if (it.r.bottom() > cur->r.y + 4) continue; along = -dy; across = dx; }
				float score = along + std::fabs(across) * 2.2f;
				if (score < bestScore) { bestScore = score; best = &it; }
			}
			if (best) focus = best->id;
			else navUnhandled = dir;
		}
	}

	if (focus != lastFocus && focus != 0) audio::play(audio::Sfx::Hover);
	lastFocus = focus;
}

void Ui::endFrame() {}

bool Ui::item(Id id, Rect r) {
	items.push_back({id, r, layer});
	bool interactive = inputEnabled && layer == activeLayer;

	// Items that appear under a resting pointer pick up focus too.
	if (interactive && usePointer && in.pointerValid && focus == 0 && r.contains(in.px, in.py)) focus = id;

	float& a = anim[id];
	a = approach(a, (interactive && focus == id) ? 1.0f : 0.0f, 16.0f, dt);

	if (!interactive || activationUsed || focus != id || !(in.pressed & BTN_CONFIRM)) return false;
	if (usePointer && !(in.pointerValid && r.contains(in.px, in.py))) return false;
	activationUsed = true;
	audio::play(audio::Sfx::Select);
	return true;
}

float Ui::hover(Id id) const {
	auto it = anim.find(id);
	return it == anim.end() ? 0.0f : easeOutCubic(it->second);
}
