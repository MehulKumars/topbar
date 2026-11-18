// Colours from the wallpaper (off by default): the bar's palette is derived from the desktop
// picture, like pywal / wallust do for Waybar, and follows it when the wallpaper changes.
#pragma once
#include "common.h"

struct Theme;

struct WallpaperSource {
    std::wstring path;      // image file; empty = a solid colour desktop
    std::wstring fallback;  // Windows' own JPEG/PNG copy, if `path` can't be decoded
    COLORREF solid = 0;
};

// Cheap fingerprint of the current wallpaper (path + file times); changes when the picture does.
std::wstring WallpaperSignature();
// Which picture is on the primary monitor. UI thread (uses the shell's wallpaper object).
WallpaperSource CurrentWallpaper();
// Decodes a small copy of the picture and builds a dark theme from its colours. `tint` (0..1) is
// how strongly the background and pills take the picture's colour. Worker thread.
bool ThemeFromWallpaper(const WallpaperSource& src, const Theme& base, float tint, Theme& out);

Theme LerpTheme(const Theme& a, const Theme& b, float t);
