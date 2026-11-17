#pragma once
#include "common.h"

class Renderer;

struct WinItem {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    std::wstring exe;           // lowercase image name, e.g. "chrome.exe"
    HMONITOR monitor = nullptr;
    bool minimized = false;
    bool flashing = false;
    ComPtr<ID2D1Bitmap> icon;
    unsigned iconGeneration = 0;
    bool iconDirty = true;
};

// Tracks the windows that would get a taskbar button, in a stable order.
class WindowList {
public:
    // Returns true if anything visible changed.
    bool Refresh();
    void UpdateIcons(Renderer& r);
    void MarkIconDirty(HWND h);
    bool UpdateTitle(HWND h);  // true if it changed
    void SetFlashing(HWND h, bool on);
    HWND Active() const { return active_; }
    WinItem* Find(HWND h);

    std::vector<WinItem> items;

private:
    HWND active_ = nullptr;
};

bool IsAppWindow(HWND h);
void ActivateOrMinimize(HWND h);
