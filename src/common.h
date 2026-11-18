// TopBar - a lightweight, Waybar-style status bar for Windows.
// Shared includes and small helpers.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define TOPBAR_NAME L"TopBar"
#define TOPBAR_VERSION L"0.1.0"

// Private window messages (sent to the hidden controller window).
enum : UINT {
    WM_APP_APPBAR = WM_APP + 1,   // appbar notifications (per bar window)
    WM_APP_AUDIO,                 // volume/mute changed
    WM_APP_AUDIO_DEVICE,          // default audio device changed
    WM_APP_NETCHANGE,             // network interface changed
    WM_APP_COMMAND,               // command from a second instance (wParam = command id)
    WM_APP_PET,                   // pet sprite decoded (wParam = generation, lParam = PetLoad*)
    WM_APP_WALLPAPER,             // wallpaper palette ready (wParam = generation, lParam = WallpaperResult*)
};

enum class RemoteCommand : WPARAM { Exit = 1, Toggle = 2, Reload = 3, Snapshot = 4 /* test builds only */ };

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// printf-style formatting into a std::wstring.
std::wstring Fmt(const wchar_t* fmt, ...);

// "1.2 GB", "512 MB" ... (base 1024)
std::wstring FormatBytes(uint64_t bytes, int decimals = 1);
// "1.2 MB/s" style rates. Bits mode gives "12 Mb/s".
std::wstring FormatRate(double bytesPerSecond, bool bits);
// "3h 12m", "2d 4h"
std::wstring FormatDuration(uint64_t seconds);

std::wstring ToLower(std::wstring s);
std::wstring Trim(const std::wstring& s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);

std::wstring ExeDirectory();
std::wstring ExePath();
std::wstring ProcessImageName(DWORD pid);       // "chrome.exe" (lowercase)
std::wstring ProcessImagePath(DWORD pid);       // full path

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    D2D1_COLOR_F d2d() const { return D2D1::ColorF(r, g, b, a); }
    Color WithAlpha(float na) const { Color c = *this; c.a = na; return c; }
    static Color Lerp(const Color& x, const Color& y, float t) {
        return { x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t };
    }
};
bool ParseColor(const std::wstring& s, Color& out);

// High resolution clock in seconds.
double NowSeconds();

// Runs `job` once on a short-lived, low-priority worker thread with COM (MTA) initialised.
// The job must only use what it captured by value and report back with PostMessage.
bool RunInBackground(std::function<void()> job);

// Ease-out cubic, and Windows' "Show animations" setting (Settings > Accessibility > Visual effects).
inline float EaseOut(float t) { t = Clamp01(t); float u = 1.f - t; return 1.f - u * u * u; }
bool SystemAnimationsEnabled();

void DebugLog(const wchar_t* fmt, ...);
