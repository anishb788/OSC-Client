#pragma once
#include "core/Types.h"

// Visual language: bright white surfaces, soft shadows, light-blue accents,
// rounded pill buttons. All shapes are drawn procedurally (no ripped assets).
namespace theme {
inline constexpr Color bgTop{255, 255, 255};
inline constexpr Color bgBottom{226, 233, 239};
inline constexpr Color stripe{120, 140, 160, 14};

inline constexpr Color accent{52, 190, 237};
inline constexpr Color accentDark{0, 148, 204};
inline constexpr Color accentSoft{222, 244, 253};
inline constexpr Color iconBlue{28, 155, 211}; // #1C9BD3: the flat category pictograms on Home
inline constexpr Color orange{217, 96, 11};    // #D9600B: the "N updates" badge

inline constexpr Color text{62, 68, 74};
inline constexpr Color textMuted{128, 136, 144};
inline constexpr Color white{255, 255, 255};

inline constexpr Color card{255, 255, 255};
inline constexpr Color border{202, 210, 218};
inline constexpr Color panel{243, 246, 249};
inline constexpr Color bar{234, 239, 244};
inline constexpr Color barLine{206, 214, 222};

inline constexpr Color warn{240, 146, 32};
inline constexpr Color muted{150, 158, 166};

// Glossy button look (user decision 2026-11-24: every button is glossy, cards stay matte). Drawn by
// Gfx::glossyRoundRect: a vertical three-stop gradient (`mid` at 55 %), a white shine band over the top ~42 %, a
// darker 3 px bottom edge and a 1.5 px outline. Reference: the approved design canvas (.pill / .primary / .destructive).
struct Gloss {
	Color top, mid, bottom;    // gradient stops at 0 %, 55 %, 100 %
	Color border;              // outline; a hovered button eases it toward the accent, or toward white (outlineToWhite)
	Uint8 shineTop, shineBottom; // alpha of the white shine band at its top and bottom edge
	Color text;                // label and (unless overridden) icon colour
	Color textShadow;          // alpha 0 = none; Primary / Destructive get a subtle dark shadow for contrast
	Color hoverTo;             // the body eases this much toward hoverTo while hovered / focused
	float hoverAmount;
	bool outlineToWhite;       // coloured presets: a blue outline would vanish on a blue body, so they ease to white
};
inline constexpr Gloss glossNormal{{255, 255, 255}, {242, 246, 249}, {226, 233, 239}, {195, 205, 214}, 217, 0,
                                   text, {0, 0, 0, 0}, accentSoft, 0.6f, false};
inline constexpr Gloss glossPrimary{{108, 203, 242}, {28, 155, 211}, {11, 130, 184}, {11, 127, 179}, 140, 10,
                                    white, {0, 50, 80, 89}, white, 0.18f, true};
// The active sort pill / latched Shift key: the Primary blue, a shade deeper with a weaker shine, so it reads as
// "on" next to a Primary action button (Search's Shift key sits beside the Search key).
inline constexpr Gloss glossSelected{{70, 176, 224}, {16, 132, 188}, {6, 108, 158}, {5, 96, 142}, 90, 6,
                                     white, {0, 40, 70, 89}, white, 0.14f, true};
inline constexpr Gloss glossDestructive{{240, 138, 128}, {217, 58, 43}, {180, 35, 24}, {154, 30, 20}, 128, 10,
                                        white, {80, 10, 0, 89}, white, 0.18f, true};
// There is no Disabled preset: nothing in M2 is disabled. Add one here when a button needs it.

inline constexpr int topBarH = 56;
inline constexpr int bottomBarH = 64;
inline constexpr int margin = 24;
} // namespace theme
