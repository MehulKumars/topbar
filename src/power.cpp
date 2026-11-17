// The power menu: a Windows copy of the wlogout menu on the Linux (Hyprland) setup, so both desktops
// look and behave the same. The screen dims, six icons sit in a row with their names, and hovering one
// cross-fades it to its coloured icon while it springs bigger and its name turns pink.
// Icons, colours, layout, keys and timing come from the wlogout style in the hyprland-dotfiles repo
// (config/wlogout), measured against a recording of the Linux menu at 2560x1440.
#include "power.h"
#include "app.h"
#include "../res/resource.h"
#include <powrprof.h>
#include <powerbase.h>

extern bool g_debugLog;

namespace {

enum class Action { None, Lock, Reboot, Shutdown, Logout, Suspend, Hibernate };

// A CSS cubic-bezier() timing function: maps time (0..1) to progress, which may overshoot 1.
struct CubicBezier {
    float x1, y1, x2, y2;
    float Eval(float t) const {
        if (t <= 0.f) return 0.f;
        if (t >= 1.f) return 1.f;
        auto bx = [&](float s) { float u = 1 - s; return 3 * u * u * s * x1 + 3 * u * s * s * x2 + s * s * s; };
        auto dx = [&](float s) {
            float u = 1 - s;
            return 3 * u * u * x1 + 6 * u * s * (x2 - x1) + 3 * s * s * (1 - x2);
        };
        // Solve x(s) = t with Newton steps, falling back to bisection, then return y(s).
        float s = t;
        for (int i = 0; i < 8; ++i) {
            float err = bx(s) - t, d = dx(s);
            if (std::fabs(err) < 1e-5f || std::fabs(d) < 1e-6f) break;
            s -= err / d;
        }
        if (s < 0.f || s > 1.f || std::fabs(bx(s) - t) > 1e-4f) {
            float lo = 0.f, hi = 1.f;
            for (int i = 0; i < 32; ++i) {
                s = (lo + hi) / 2;
                (bx(s) < t ? lo : hi) = s;
            }
        }
        float u = 1 - s;
        return 3 * u * u * s * y1 + 3 * u * s * s * y2 + s * s * s;
    }
};

// wlogout style.css: "transition: all 0.3s cubic-bezier(.55, 0.0, .28, 1.682)" (overshoots ~16%).
constexpr CubicBezier kHoverCurve{ 0.55f, 0.f, 0.28f, 1.682f };
constexpr float kHoverSeconds = 0.3f;
// Hyprland fades the menu's layer in and out with its "fade" animation: speed 3 (0.3 s), bezier smoothOut.
constexpr CubicBezier kFadeCurve{ 0.5f, 0.f, 0.99f, 0.99f };
constexpr float kFadeSeconds = 0.3f;

// The menu's geometry on the 2560x1440 monitor; other monitor sizes scale from it.
constexpr float kRefW = 2560.f, kRefH = 1440.f;
constexpr float kSideMargin = 230.f;   // wlogout's default left/right margin
constexpr float kTopMargin = 400.f;    // Wlogout.sh at 1440p: -T 400 -B 400
constexpr float kHoverMargin = 30.f;   // button:hover { margin: 30px }
constexpr float kLabelSize = 32.f;     // window { font-size: 24pt }
constexpr float kLabelAlign = 0.9f;    // wlogout sets each label 90% of the way down its button
constexpr float kIconSmall = 0.2f, kIconBig = 0.5f;  // background-size: 20%, 50% on hover and focus

// Colours of the wlogout style: window background rgba(30, 30, 46, 0.8), text @foreground,
// hover text @color13 (from the wallust palette in the dotfiles).
constexpr BYTE kBackdropAlpha = 204;
constexpr COLORREF kBackdropRgb = RGB(30, 30, 46);
constexpr Color kText{ 0xF9 / 255.f, 0xE3 / 255.f, 0xDF / 255.f, 1.f };
constexpr Color kHoverText{ 0xA0 / 255.f, 0x53 / 255.f, 0x6C / 255.f, 1.f };

// One animated CSS property. Like GTK, a new target restarts the 0.3 s transition from the current value.
struct Tween {
    float from = 0, to = 0;
    double start = -1;
    void Reset(float v) { from = to = v; start = -1; }
    void Set(float v, double now) {
        if (v == to) return;
        from = Value(now);
        to = v;
        start = now;
    }
    float Value(double now) const {
        if (start < 0) return to;
        float t = (float)((now - start) / kHoverSeconds);
        return t >= 1.f ? to : from + (to - from) * kHoverCurve.Eval(t);
    }
    bool Busy(double now) const { return start >= 0 && now - start < kHoverSeconds; }
};

constexpr int kLevels = 3;  // each icon is pre-scaled to its resting sizes and one in between

struct Button {
    Action action;
    const wchar_t* label;
    WPARAM key;         // wlogout keybind (layout file)
    int icon, hoverIcon;
    float labelW = 0;   // natural width of the label
    float baseW = 0;    // column width while nothing is hovered
    Tween margin, size, swap, tint;
    ComPtr<ID2D1Bitmap> normal[kLevels], hover[kLevels];
    int normalPx[kLevels] = {}, hoverPx[kLevels] = {};
    bool hoverLoaded = false;
    D2D1_RECT_F col{}, box{};  // this frame: the grid column, and the button inside its margin
};

class Overlay {
public:
    Overlay(App& app, const MonitorDesc& mon) : app_(app), mon_(mon) {}
    Action Run();

    static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    bool Create();
    void LoadIcon(Button& b, bool hover);
    bool LoadPendingIcons();
    void Destroy();
    void Layout(double now);
    void Update(double now);
    void Render(double now);
    void Present(BYTE alpha);
    int HitTest() const;
    void MouseDown();
    void MouseUp();
    void Key(WPARAM vk);
    void Trigger(int i);
    void Close();
    float Fade(double now) const;
    bool TweensBusy(double now) const;

    App& app_;
    MonitorDesc mon_;
    std::vector<Button> buttons_;
    HWND backdrop_ = nullptr, panel_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    ComPtr<IDWriteTextFormat> labelFont_;
    int monW_ = 0, monH_ = 0;
    float side_ = 0, areaTop_ = 0, areaBottom_ = 0, hoverMargin_ = 0, labelH_ = 0, labelSum_ = 0;
    int panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;  // panel window, relative to the monitor
    double openAt_ = 0, closeAt_ = 0;
    float closeFrom_ = 1.f;
    bool closing_ = false, done_ = false, wasBusy_ = false;
    int hoverIdx_ = -1, focusIdx_ = 0, pressed_ = -1;
    bool backdropDown_ = false;
    int lastBackdrop_ = -1, lastPanel_ = -1;
    Action chosen_ = Action::None;
    // --debug stats
    int frames_ = 0;
    double renderTime_ = 0, presentTime_ = 0;
};

bool g_open = false;

Overlay* FromWindow(HWND h) { return (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA); }

bool Overlay::Create() {
    // Same buttons, order, names and keys as the wlogout layout file.
    buttons_.push_back({ Action::Lock, L"Lock", 'L', IDR_POWER_LOCK, IDR_POWER_LOCK_HOVER });
    buttons_.push_back({ Action::Reboot, L"Reboot", 'R', IDR_POWER_REBOOT, IDR_POWER_REBOOT_HOVER });
    buttons_.push_back({ Action::Shutdown, L"Shutdown", 'S', IDR_POWER_SHUTDOWN, IDR_POWER_SHUTDOWN_HOVER });
    buttons_.push_back({ Action::Logout, L"Logout", 'E', IDR_POWER_LOGOUT, IDR_POWER_LOGOUT_HOVER });
    buttons_.push_back({ Action::Suspend, L"Suspend", 'U', IDR_POWER_SUSPEND, IDR_POWER_SUSPEND_HOVER });
    if (IsPwrHibernateAllowed())
        buttons_.push_back({ Action::Hibernate, L"Hibernate", 'H', IDR_POWER_HIBERNATE, IDR_POWER_HIBERNATE_HOVER });

    // Geometry in physical pixels of this monitor.
    Renderer& r = app_.renderer;
    monW_ = mon_.rect.right - mon_.rect.left;
    monH_ = mon_.rect.bottom - mon_.rect.top;
    float u = std::min(monH_ / kRefH, monW_ / kRefW);  // portrait or narrow screens scale by width
    side_ = std::round(kSideMargin * monW_ / kRefW);
    areaTop_ = std::round(kTopMargin * u);
    areaBottom_ = (float)monH_ - areaTop_;
    hoverMargin_ = kHoverMargin * u;
    labelFont_ = r.CreateTextFormat(kLabelSize * u);
    labelH_ = r.LineHeight(labelFont_.Get());
    // GTK's grid gives each column its label's width plus an equal share of the space left over.
    labelSum_ = 0;
    for (auto& b : buttons_) {
        b.labelW = r.Measure(b.label, labelFont_.Get());
        labelSum_ += b.labelW;
    }
    float n = (float)buttons_.size();
    float share = (monW_ - 2 * side_ - labelSum_) / n;
    float maxW = 0;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        Button& b = buttons_[i];
        b.baseW = b.labelW + share;
        maxW = std::max(maxW, b.baseW);
        b.margin.Reset(0.f);
        b.size.Reset((int)i == focusIdx_ ? kIconBig : kIconSmall);  // GTK focuses the first button
        b.swap.Reset(0.f);
        b.tint.Reset(0.f);
    }
    // The panel only covers the row (icons at their biggest overshoot down to the labels), not the screen.
    float cy = (areaTop_ + areaBottom_) / 2;
    panelX_ = std::max(0, (int)side_ - 2);
    panelW_ = monW_ - 2 * panelX_;
    panelY_ = std::max(0, (int)std::floor(cy - 0.3f * maxW) - 2);
    panelH_ = std::min(monH_, (int)std::ceil(areaBottom_) + 2) - panelY_;
    if (panelW_ <= 0 || panelH_ <= 0) return false;

    // The plain icons are needed for the first frame; the coloured hover ones load during the fade-in.
    double t0 = NowSeconds();
    for (auto& b : buttons_) LoadIcon(b, false);
    if (g_debugLog) DebugLog(L"power: icons loaded in %.1f ms", (NowSeconds() - t0) * 1000);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = app_.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(kBackdropRgb);  // lives as long as the process
        wc.lpfnWndProc = BackdropProc;
        wc.lpszClassName = L"TopBarPowerBackdrop";
        RegisterClassExW(&wc);
        wc.hbrBackground = nullptr;
        wc.lpfnWndProc = PanelProc;
        wc.lpszClassName = L"TopBarPowerPanel";
        RegisterClassExW(&wc);
        registered = true;
    }
    backdrop_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"TopBarPowerBackdrop", L"Power",
                                WS_POPUP, mon_.rect.left, mon_.rect.top, monW_, monH_, nullptr, nullptr, app_.inst,
                                nullptr);
    if (!backdrop_) return false;
    SetWindowLongPtrW(backdrop_, GWLP_USERDATA, (LONG_PTR)this);
    SetLayeredWindowAttributes(backdrop_, 0, 0, LWA_ALPHA);
    panel_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"TopBarPowerPanel",
                             L"", WS_POPUP, mon_.rect.left + panelX_, mon_.rect.top + panelY_, panelW_, panelH_,
                             backdrop_, nullptr, app_.inst, nullptr);
    if (!panel_) return false;
    SetWindowLongPtrW(panel_, GWLP_USERDATA, (LONG_PTR)this);
    BOOL yes = TRUE;
    DwmSetWindowAttribute(backdrop_, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));
    DwmSetWindowAttribute(panel_, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));

    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), panelW_, -panelH_, 1, 32, BI_RGB };
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_) return false;
    oldBmp_ = SelectObject(memDC_, dib_);

    // First frame, fully transparent; the fade-in starts from here.
    double now = NowSeconds();
    Layout(now);
    Render(now);
    Present(0);
    lastPanel_ = 0;
    lastBackdrop_ = 0;
    ShowWindow(backdrop_, SW_SHOW);
    ShowWindow(panel_, SW_SHOWNOACTIVATE);
    SetForegroundWindow(backdrop_);
    return true;
}

// Each icon is decoded once and pre-scaled to its two resting sizes plus one in between, so it is
// drawn pixel-sharp at rest and never shrunk more than ~1.6x (bilinear) while it animates.
// If an icon can't load, its button still works and shows its name.
void Overlay::LoadIcon(Button& b, bool hover) {
    float n = (float)buttons_.size();
    float w = hover ? b.baseW - 2 * hoverMargin_ / n : b.baseW;  // hovered: the margin narrows the button
    int s = (int)std::lround(kIconSmall * b.baseW);
    int big = (int)std::lround(kIconBig * w);
    int* px = hover ? b.hoverPx : b.normalPx;
    ComPtr<ID2D1Bitmap>* bmp = hover ? b.hover : b.normal;
    px[0] = s;
    px[1] = (int)std::lround(std::sqrt((float)s * big));
    px[2] = big;
    if (!app_.renderer.LoadPngResource(app_.inst, hover ? b.hoverIcon : b.icon, px, kLevels, bmp))
        for (int i = 0; i < kLevels; ++i) bmp[i].Reset();
    if (hover) b.hoverLoaded = true;
}

// Loads the hovered button's coloured icon right away, otherwise one more per frame.
// Returns true while some are still to load.
bool Overlay::LoadPendingIcons() {
    if (hoverIdx_ >= 0 && !buttons_[hoverIdx_].hoverLoaded) LoadIcon(buttons_[hoverIdx_], true);
    for (auto& b : buttons_)
        if (!b.hoverLoaded) {
            LoadIcon(b, true);
            break;
        }
    for (auto& b : buttons_)
        if (!b.hoverLoaded) return true;
    return false;
}

void Overlay::Destroy() {
    if (panel_) { DestroyWindow(panel_); panel_ = nullptr; }
    if (backdrop_) { DestroyWindow(backdrop_); backdrop_ = nullptr; }
    if (memDC_) {
        if (oldBmp_) SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
}

// Lays the row out like GTK: a hovered button's margin widens its column (others give up an equal
// share) and shrinks the button inside it, which is what moves the label up and nudges the neighbours.
void Overlay::Layout(double now) {
    float n = (float)buttons_.size();
    float marginSum = 0;
    for (auto& b : buttons_) marginSum += 2 * b.margin.Value(now);
    float share = (monW_ - 2 * side_ - labelSum_ - marginSum) / n;
    float x = side_;
    for (auto& b : buttons_) {
        float m = b.margin.Value(now);
        float w = b.labelW + 2 * m + share;
        b.col = { x, areaTop_, x + w, areaBottom_ };
        b.box = { x + m, areaTop_ + m, x + w - m, areaBottom_ - m };
        x += w;
    }
}

int Overlay::HitTest() const {
    POINT pt;
    GetCursorPos(&pt);
    float x = (float)(pt.x - mon_.rect.left), y = (float)(pt.y - mon_.rect.top);
    if (y < areaTop_ || y >= areaBottom_) return -1;
    for (int i = 0; i < (int)buttons_.size(); ++i)
        if (x >= buttons_[i].col.left && x < buttons_[i].col.right) return i;
    return -1;
}

void Overlay::Update(double now) {
    Layout(now);
    if (!closing_) {  // once closing, the menu fades out as it was
        hoverIdx_ = HitTest();
        for (int i = 0; i < (int)buttons_.size(); ++i) {
            Button& b = buttons_[i];
            bool hov = i == hoverIdx_, foc = i == focusIdx_;
            b.margin.Set(hov ? hoverMargin_ : 0.f, now);
            b.size.Set(hov || foc ? kIconBig : kIconSmall, now);
            b.swap.Set(hov ? 1.f : 0.f, now);
            b.tint.Set(hov ? 1.f : 0.f, now);
        }
    }
    if (closing_ && now - closeAt_ >= kFadeSeconds) done_ = true;
}

bool Overlay::TweensBusy(double now) const {
    for (auto& b : buttons_)
        if (b.margin.Busy(now) || b.size.Busy(now) || b.swap.Busy(now) || b.tint.Busy(now)) return true;
    return false;
}

float Overlay::Fade(double now) const {
    if (closing_) return closeFrom_ * (1.f - kFadeCurve.Eval((float)((now - closeAt_) / kFadeSeconds)));
    return kFadeCurve.Eval((float)((now - openAt_) / kFadeSeconds));
}

void DrawIcon(Renderer& r, const ComPtr<ID2D1Bitmap>* levels, const int* px, float cx, float cy, float d,
              float opacity) {
    if (d < 1.f || opacity < 0.002f) return;
    int size = (int)std::lround(d);
    int pick = kLevels - 1;  // smallest pre-scaled size that is at least as big, else the biggest
    for (int i = 0; i < kLevels; ++i)
        if (px[i] >= size) { pick = i; break; }
    if (!levels[pick]) return;
    float left = std::round(cx - size / 2.f), top = std::round(cy - size / 2.f);
    r.Bitmap(levels[pick].Get(), { left, top, left + size, top + size }, std::min(opacity, 1.f));
}

void Overlay::Render(double now) {
    Renderer& r = app_.renderer;
    if (!r.Begin(memDC_, panelW_, panelH_, 96.f, true)) return;  // 96 dpi: one unit is one pixel
    r.Clear(Color{ 0, 0, 0, 0 });
    r.SetTransform(D2D1::Matrix3x2F::Translation(-(float)panelX_, -(float)panelY_));  // monitor coordinates
    for (auto& b : buttons_) {
        float bw = b.box.right - b.box.left, bh = b.box.bottom - b.box.top;
        float cx = (b.box.left + b.box.right) / 2, cy = (b.box.top + b.box.bottom) / 2;
        float d = b.size.Value(now) * bw;
        float p = Clamp01(b.swap.Value(now));  // GTK cross-fades the background image
        DrawIcon(r, b.normal, b.normalPx, cx, cy, d, 1.f - p);
        DrawIcon(r, b.hover, b.hoverPx, cx, cy, d, p);
        float ly = std::round(b.box.top + (bh - labelH_) * kLabelAlign);
        Color c = Color::Lerp(kText, kHoverText, Clamp01(b.tint.Value(now)));
        r.Text(b.label, labelFont_.Get(), { b.col.left - 40.f, ly, b.col.right + 40.f, ly + labelH_ }, c, Align::Center);
    }
    r.ResetTransform();
    r.End();
}

void Overlay::Present(BYTE alpha) {
    POINT src{ 0, 0 }, pos{ mon_.rect.left + panelX_, mon_.rect.top + panelY_ };
    SIZE size{ panelW_, panelH_ };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(panel_, screen, &pos, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void Overlay::Trigger(int i) {
    if (closing_ || i < 0 || i >= (int)buttons_.size()) return;
    chosen_ = buttons_[i].action;
    Close();
}

void Overlay::Close() {
    if (closing_) return;
    double now = NowSeconds();
    closeFrom_ = Fade(now);
    closing_ = true;
    closeAt_ = now;
    pressed_ = -1;
}

void Overlay::MouseDown() {
    if (closing_) return;
    pressed_ = HitTest();
    if (pressed_ < 0) { backdropDown_ = true; return; }
    focusIdx_ = pressed_;  // GTK buttons take focus on click
}

void Overlay::MouseUp() {
    int over = HitTest();
    if (pressed_ >= 0 && over == pressed_) Trigger(pressed_);   // a click, like a GTK button
    else if (backdropDown_ && over < 0) Close();                // click on the dimmed background
    pressed_ = -1;
    backdropDown_ = false;
}

void Overlay::Key(WPARAM vk) {
    if (closing_) return;
    int n = (int)buttons_.size();
    switch (vk) {
    case VK_ESCAPE: Close(); return;
    case VK_LEFT: case VK_UP: focusIdx_ = std::max(0, focusIdx_ - 1); return;
    case VK_RIGHT: case VK_DOWN: focusIdx_ = std::min(n - 1, focusIdx_ + 1); return;
    case VK_TAB: focusIdx_ = (focusIdx_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? n - 1 : 1)) % n; return;
    case VK_RETURN: case VK_SPACE: Trigger(focusIdx_); return;
    }
    if (GetKeyState(VK_CONTROL) & 0x8000) return;
    for (int i = 0; i < n; ++i)
        if (vk == buttons_[i].key) { Trigger(i); return; }
}

Action Overlay::Run() {
    if (!Create()) { Destroy(); return Action::None; }
    openAt_ = NowSeconds();
    bool quit = false;
    int quitCode = 0;
    while (!done_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; quitCode = (int)msg.wParam; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        double now = NowSeconds();
        Update(now);
        bool loading = !closing_ && LoadPendingIcons();
        bool busy = TweensBusy(now);
        bool redraw = busy || wasBusy_;  // one more frame after a transition ends, at its final value
        wasBusy_ = busy;
        double t0 = NowSeconds();
        if (redraw) Render(now);
        double t1 = NowSeconds();
        float fade = Fade(now);
        int backdropA = (int)std::lround(kBackdropAlpha * fade), panelA = (int)std::lround(255.f * fade);
        if (backdropA != lastBackdrop_) {
            SetLayeredWindowAttributes(backdrop_, 0, (BYTE)backdropA, LWA_ALPHA);
            lastBackdrop_ = backdropA;
        }
        if (redraw || panelA != lastPanel_) {
            Present((BYTE)panelA);
            lastPanel_ = panelA;
        }
        if (redraw && g_debugLog) {
            ++frames_;
            renderTime_ += t1 - t0;
            presentTime_ += NowSeconds() - t1;
        }
        if (done_) break;
        bool fading = closing_ || now - openAt_ < kFadeSeconds + 0.02;
        if (fading || busy || redraw || loading) {
            // Pace frames to the display refresh, but no faster than ~80 fps on high-refresh screens.
            double frameStart = now;
            do {
                if (FAILED(DwmFlush())) { Sleep(8); break; }
            } while (NowSeconds() - frameStart < 0.012);
        } else {
            WaitMessage();  // idle: nothing moves until the mouse or a key does
        }
    }
    if (g_debugLog && frames_)
        DebugLog(L"power: %d frames, render %.2f ms, present %.2f ms (avg), panel %dx%d", frames_,
                 renderTime_ * 1000 / frames_, presentTime_ * 1000 / frames_, panelW_, panelH_);
    Destroy();
    if (quit) PostQuitMessage(quitCode);
    return quit ? Action::None : chosen_;
}

LRESULT CALLBACK Overlay::BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Overlay* o = FromWindow(h);
    if (o) {
        switch (m) {
        case WM_LBUTTONDOWN: SetCapture(h); o->MouseDown(); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); o->MouseUp(); return 0;
        case WM_RBUTTONUP: o->Close(); return 0;
        case WM_KEYDOWN: o->Key(w); return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) o->Close();  // Win key, Alt+Tab, another app: just go away
            break;
        case WM_CLOSE: o->Close(); return 0;           // Alt+F4
        }
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Overlay::PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Overlay* o = FromWindow(h);
    if (o) {
        switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: SetCapture(h); o->MouseDown(); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); o->MouseUp(); return 0;
        case WM_RBUTTONUP: o->Close(); return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void EnableShutdownPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(tok);
}

void SleepPC(App& app) {
    // Modern Standby PCs (S0 low power idle) have no S3 sleep, and SetSuspendState(FALSE) hibernates
    // them when hibernation is on. Turning the display off is how Windows itself enters Modern Standby.
    SYSTEM_POWER_CAPABILITIES caps{};
    if (CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &caps, sizeof(caps)) == 0 && caps.AoAc) {
        app.TurnDisplayOffSoon();
        return;
    }
    EnableShutdownPrivilege();
    SetSuspendState(FALSE, FALSE, FALSE);
}

bool PowerOverlay::IsOpen() { return g_open; }

void PowerOverlay::Run(App& app, const MonitorDesc& mon) {
    if (g_open) return;
    g_open = true;
    Action action;
    {
        Overlay o(app, mon);
        action = o.Run();
    }
    g_open = false;
    // The menu is used rarely: drop its drawing surface and icons and give the memory back to Windows.
    app.renderer.ReleaseAlphaTarget();
    app.TrimMemorySoon();
#ifdef TOPBAR_DRY_RUN  // test builds: log the choice instead of locking / restarting / shutting down
    DebugLog(L"power: chose action %d (dry run, not executed)", (int)action);
    return;
#endif
    switch (action) {
    case Action::Lock: LockWorkStation(); break;
    case Action::Reboot: EnableShutdownPrivilege(); ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_FLAG_PLANNED); break;
    case Action::Shutdown: EnableShutdownPrivilege(); ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_FLAG_PLANNED); break;
    case Action::Logout: ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); break;
    case Action::Suspend: SleepPC(app); break;
    case Action::Hibernate: EnableShutdownPrivilege(); SetSuspendState(TRUE, FALSE, FALSE); break;
    case Action::None: break;
    }
}
