#pragma once

namespace vertex::chrome_theme {

struct Rgb {
    int r;
    int g;
    int b;
};

inline constexpr Rgb Ink{23, 25, 31};
inline constexpr Rgb Panel{245, 246, 248};
inline constexpr Rgb Rail{229, 232, 238};
inline constexpr Rgb Active{255, 255, 255};
inline constexpr Rgb Hover{249, 251, 255};
inline constexpr Rgb Pressed{222, 228, 240};
inline constexpr Rgb Disabled{236, 239, 244};
inline constexpr Rgb Accent{59, 109, 246};
inline constexpr Rgb AccentSoft{221, 231, 255};
inline constexpr Rgb Quiet{138, 147, 165};
inline constexpr Rgb DisabledText{166, 173, 186};
inline constexpr Rgb Line{209, 214, 224};

inline constexpr int TabHeight = 38;
inline constexpr int ToolbarHeight = 46;
inline constexpr int StatusHeight = 24;
inline constexpr int ButtonWidth = 34;
inline constexpr int ButtonHeight = 30;
inline constexpr int Margin = 8;
inline constexpr int Gap = 5;
inline constexpr int CornerRadius = 8;

}  // namespace vertex::chrome_theme
