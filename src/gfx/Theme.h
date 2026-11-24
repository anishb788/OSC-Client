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

inline constexpr int topBarH = 56;
inline constexpr int bottomBarH = 64;
inline constexpr int margin = 24;
} // namespace theme
