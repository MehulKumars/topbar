// The power menu: a dimmed screen with big round buttons that spring in, grow and glow on hover.
// Destructive actions (sign out, restart, shut down) need a press-and-hold so they never fire by accident.
#include "power.h"
#include "app.h"
#include <powrprof.h>

namespace {

enum class Action { None, Lock, Sleep, Hibernate, DisplayOff, SignOut, Restart, Shutdown };

// Damped spring: gives the bouncy "grow" feel. Semi-implicit Euler with small sub-steps.
struct Spring {
    float x = 0, v = 0, target = 0;
    void Step(float dt, float k, float c) {
        const int n = 4;
        float h = dt / n;
        for (int i = 0; i < n; ++i) {
            float a = k * (target - x) - c * v;
            v += a * h;
            x += v * h;
        }
    }
    bool Settled() const { return std::fabs(target - x) < 0.002f && std::fabs(v) < 0.002f; }
};

struct Button {
    Action action;
    wchar_t glyph;
    const wchar_t* label;
    Color color;
    bool hold;
    Spring appear, hover, press, flash;
    float holdProgress = 0;
    float spin = 0;
    float delay = 0;
};

constexpr float kRadius = 46.f, kGapX = 38.f, kSidePad = 70.f, kPanelH = 350.f, kButtonsY = 196.f;
constexpr float kHoldSeconds = 0.65f;
constexpr BYTE kBackdropAlpha = 195;

class Overlay {
public:
    Overlay(App& app, const MonitorDesc& mon) : app_(app), mon_(mon) {}
    Action Run();

    static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    bool Create();
    void Destroy();
    void Update(float dt);
    void Render();
    int HitTest() const;
    void MouseDown();
    void MouseUp();
    void Key(WPARAM vk, bool down, bool repeat);
    void Trigger(int i);
    void Close();
    D2D1_POINT_2F Center(int i) const;
    bool Animating() const;

    App& app_;
    MonitorDesc mon_;
    std::vector<Button> buttons_;
    HWND backdrop_ = nullptr, panel_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    float scale_ = 1.f;
    int panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;
    float wDip_ = 0;
    double openAt_ = 0, closeAt_ = 0;
    bool closing_ = false, done_ = false;
    int hoverIdx_ = -1, pressed_ = -1, holding_ = -1, keyIdx_ = -1;
    bool keyboardMode_ = false, keyHold_ = false, backdropDown_ = false;
    POINT lastCursor_{ -1, -1 };
    BYTE lastAlpha_ = 0;
    Action chosen_ = Action::None;
    std::wstring greeting_, subtitle_;
};

bool g_open = false;

Overlay* FromWindow(HWND h) { return (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA); }

float EaseOutCubic(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }

D2D1_POINT_2F Overlay::Center(int i) const {
    float n = (float)buttons_.size();
    float rowW = n * kRadius * 2 + (n - 1) * kGapX;
    float x0 = (wDip_ - rowW) / 2 + kRadius;
    return { x0 + i * (kRadius * 2 + kGapX), kButtonsY };
}

bool Overlay::Create() {
    const Theme& t = app_.cfg.theme;
    Color lavender;
    ParseColor(L"#b4befe", lavender);
    buttons_.push_back({ Action::Lock, 0xE72E, L"Lock", t.accent, false });
    buttons_.push_back({ Action::Sleep, 0xE708, L"Sleep", t.memory, false });
    if (IsPwrHibernateAllowed()) buttons_.push_back({ Action::Hibernate, 0xED39, L"Hibernate", lavender, false });
    buttons_.push_back({ Action::DisplayOff, 0xEA14, L"Display off", t.network, false });
    buttons_.push_back({ Action::SignOut, 0xF3B1, L"Sign out", t.volume, true });
    buttons_.push_back({ Action::Restart, 0xE777, L"Restart", t.temp, true });
    buttons_.push_back({ Action::Shutdown, 0xE7E8, L"Shut down", t.hot, true });
    for (size_t i = 0; i < buttons_.size(); ++i) buttons_[i].delay = 0.05f + 0.045f * (float)i;

    // Header text.
    SYSTEMTIME st;
    GetLocalTime(&st);
    greeting_ = st.wHour < 5 ? L"Good night" : st.wHour < 12 ? L"Good morning" : st.wHour < 17 ? L"Good afternoon"
              : st.wHour < 22 ? L"Good evening" : L"Good night";
    wchar_t tm[64] = L"";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, tm, _countof(tm));
    subtitle_ = std::wstring(tm) + L"   ·   up " + FormatDuration(GetTickCount64() / 1000);
    if (app_.stats.battery.present && app_.stats.battery.percent >= 0)
        subtitle_ += Fmt(L"   ·   battery %d%%%s", app_.stats.battery.percent, app_.stats.battery.charging ? L" (charging)" : L"");

    scale_ = mon_.dpi / 96.f;
    float n = (float)buttons_.size();
    wDip_ = std::max(640.f, n * kRadius * 2 + (n - 1) * kGapX + kSidePad * 2);
    panelW_ = (int)std::ceil(wDip_ * scale_);
    panelH_ = (int)std::ceil(kPanelH * scale_);
    int monW = mon_.rect.right - mon_.rect.left, monH = mon_.rect.bottom - mon_.rect.top;
    panelW_ = std::min(panelW_, monW);
    panelX_ = mon_.rect.left + (monW - panelW_) / 2;
    panelY_ = mon_.rect.top + (int)(monH * 0.42f) - panelH_ / 2;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = app_.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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
                                WS_POPUP, mon_.rect.left, mon_.rect.top, monW, monH, nullptr, nullptr, app_.inst, nullptr);
    if (!backdrop_) return false;
    SetWindowLongPtrW(backdrop_, GWLP_USERDATA, (LONG_PTR)this);
    SetLayeredWindowAttributes(backdrop_, 0, 0, LWA_ALPHA);
    panel_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"TopBarPowerPanel",
                             L"", WS_POPUP, panelX_, panelY_, panelW_, panelH_, backdrop_, nullptr, app_.inst, nullptr);
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
    oldBmp_ = SelectObject(memDC_, dib_);

    ShowWindow(backdrop_, SW_SHOW);
    Render();
    ShowWindow(panel_, SW_SHOWNOACTIVATE);
    SetForegroundWindow(backdrop_);
    return true;
}

void Overlay::Destroy() {
    if (panel_) { DestroyWindow(panel_); panel_ = nullptr; }
    if (backdrop_) { DestroyWindow(backdrop_); backdrop_ = nullptr; }
    if (memDC_) { SelectObject(memDC_, oldBmp_); DeleteDC(memDC_); memDC_ = nullptr; }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
}

int Overlay::HitTest() const {
    if (keyboardMode_) return keyIdx_;
    POINT pt;
    GetCursorPos(&pt);
    float x = (pt.x - panelX_) / scale_, y = (pt.y - panelY_) / scale_;
    for (int i = 0; i < (int)buttons_.size(); ++i) {
        D2D1_POINT_2F c = Center(i);
        float dx = x - c.x, dy = y - c.y;
        if (dx * dx + dy * dy <= (kRadius * 1.22f) * (kRadius * 1.22f)) return i;
    }
    return -1;
}

void Overlay::Trigger(int i) {
    if (closing_ || i < 0 || i >= (int)buttons_.size()) return;
    chosen_ = buttons_[i].action;
    buttons_[i].flash.x = 0;
    buttons_[i].flash.target = 1;
    Close();
}

void Overlay::Close() {
    if (closing_) return;
    closing_ = true;
    closeAt_ = NowSeconds();
    holding_ = -1;
}

void Overlay::MouseDown() {
    keyboardMode_ = false;
    hoverIdx_ = HitTest();
    if (hoverIdx_ < 0) { backdropDown_ = true; return; }
    pressed_ = hoverIdx_;
    if (buttons_[pressed_].hold) holding_ = pressed_;
}

void Overlay::MouseUp() {
    int over = HitTest();
    if (pressed_ >= 0) {
        if (!buttons_[pressed_].hold && over == pressed_) Trigger(pressed_);
        holding_ = -1;  // released before the ring filled: cancel
        pressed_ = -1;
    } else if (backdropDown_ && over < 0) {
        Close();
    }
    backdropDown_ = false;
}

void Overlay::Key(WPARAM vk, bool down, bool repeat) {
    int n = (int)buttons_.size();
    if (!down) {
        if ((vk == VK_RETURN || vk == VK_SPACE) && keyHold_) { keyHold_ = false; holding_ = -1; pressed_ = -1; }
        return;
    }
    switch (vk) {
    case VK_ESCAPE: Close(); break;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_TAB: {
        bool back = vk == VK_LEFT || (vk == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
        if (!keyboardMode_) keyIdx_ = hoverIdx_ >= 0 ? hoverIdx_ : (back ? 0 : -1);
        keyboardMode_ = true;
        keyIdx_ = ((keyIdx_ < 0 ? (back ? 0 : -1) : keyIdx_) + (back ? n - 1 : 1)) % n;
        break;
    }
    case VK_RETURN:
    case VK_SPACE:
        if (repeat) break;
        if (keyboardMode_ && keyIdx_ >= 0) {
            if (buttons_[keyIdx_].hold) { holding_ = keyIdx_; pressed_ = keyIdx_; keyHold_ = true; }
            else Trigger(keyIdx_);
        }
        break;
    }
}

void Overlay::Update(float dt) {
    double now = NowSeconds();
    float t = (float)(now - openAt_);

    // Backdrop fade.
    float a = closing_ ? (1.f - EaseOutCubic((float)(now - closeAt_) / 0.18f)) * EaseOutCubic(t / 0.2f)
                       : EaseOutCubic(t / 0.2f);
    BYTE alpha = (BYTE)std::lround(kBackdropAlpha * Clamp01(a));
    if (alpha != lastAlpha_) {
        SetLayeredWindowAttributes(backdrop_, 0, alpha, LWA_ALPHA);
        lastAlpha_ = alpha;
    }

    // Mouse movement switches back from keyboard navigation.
    POINT pt;
    GetCursorPos(&pt);
    if (pt.x != lastCursor_.x || pt.y != lastCursor_.y) {
        if (lastCursor_.x != -1) keyboardMode_ = false;
        lastCursor_ = pt;
    }
    hoverIdx_ = closing_ ? -1 : HitTest();

    for (int i = 0; i < (int)buttons_.size(); ++i) {
        Button& b = buttons_[i];
        if (closing_) {
            b.appear.target = 0;
            b.appear.Step(dt, 520.f, 38.f);
        } else {
            b.appear.target = t >= b.delay ? 1.f : 0.f;
            b.appear.Step(dt, 240.f, 15.f);  // under-damped: overshoots, then settles = "pop"
        }
        b.hover.target = hoverIdx_ == i ? 1.f : 0.f;
        b.hover.Step(dt, 420.f, 24.f);
        b.press.target = pressed_ == i ? 1.f : 0.f;
        b.press.Step(dt, 900.f, 45.f);
        b.flash.Step(dt, 60.f, 12.f);
        if (holding_ == i) {
            b.holdProgress += dt / kHoldSeconds;
            if (b.holdProgress >= 1.f) {
                b.holdProgress = 1.f;
                holding_ = -1;
                pressed_ = -1;
                Trigger(i);
            }
        } else if (chosen_ != b.action || !closing_) {
            b.holdProgress = std::max(0.f, b.holdProgress - dt * 3.5f);
        }
        if (b.action == Action::Restart) {
            if (b.hover.x > 0.02f) {
                b.spin += dt * 320.f * b.hover.x;
            } else if (std::fmod(b.spin, 360.f) > 0.5f) {
                // Finish the turn so the arrow comes to rest upright.
                float rest = std::ceil(b.spin / 360.f) * 360.f;
                b.spin = std::min(rest, b.spin + dt * 360.f);
                if (rest - b.spin < 0.5f) b.spin = 0.f;
            } else {
                b.spin = 0.f;
            }
        }
    }
    if (closing_ && now - closeAt_ > 0.22) done_ = true;
}

bool Overlay::Animating() const {
    if (closing_ || NowSeconds() - openAt_ < 1.2 || holding_ >= 0) return true;
    for (auto& b : buttons_) {
        if (!b.appear.Settled() || !b.hover.Settled() || !b.press.Settled() || b.holdProgress > 0) return true;
        if (b.action == Action::Restart && (b.hover.x > 0.01f || b.spin != 0.f)) return true;
        if ((b.action == Action::Shutdown || b.action == Action::Sleep) && b.hover.x > 0.01f) return true;
    }
    return false;
}

void Overlay::Render() {
    Renderer& r = app_.renderer;
    const Theme& th = app_.cfg.theme;
    if (!r.Begin(memDC_, panelW_, panelH_, (float)mon_.dpi, true)) return;
    r.Clear(Color{ 0, 0, 0, 0 });
    double now = NowSeconds();
    float t = (float)(now - openAt_);
    float headA = closing_ ? Clamp01(1.f - (float)(now - closeAt_) / 0.15f) : EaseOutCubic((t - 0.02f) / 0.25f);
    float headY = (1.f - headA) * 10.f;
    Color white{ 1, 1, 1, 1 };

    r.Text(greeting_, Font::Title, { 0, 30 + headY, wDip_, 76 + headY }, th.text.WithAlpha(headA), Align::Center);
    r.Text(subtitle_, Font::Tip, { 0, 78 + headY, wDip_, 100 + headY }, th.subtext.WithAlpha(headA), Align::Center);

    for (int i = 0; i < (int)buttons_.size(); ++i) {
        const Button& b = buttons_[i];
        float ap = std::max(0.f, b.appear.x);
        float a = Clamp01(ap * 1.6f);
        if (a < 0.003f) continue;
        float hov = Clamp01(b.hover.x);
        float sc = ap * (1.f + 0.14f * b.hover.x) * (1.f - 0.07f * b.press.x);
        D2D1_POINT_2F c = Center(i);
        c.y += (1.f - std::min(1.f, ap)) * 28.f;  // rises into place
        float R = kRadius * sc;

        for (int k = 3; k >= 1; --k)
            r.FillEllipse(c, R + k * 7.f * hov, R + k * 7.f * hov, b.color.WithAlpha(0.06f * hov * a));
        r.FillEllipse(c, R, R, Color::Lerp(th.pill, b.color, 0.08f + 0.16f * hov).WithAlpha(0.97f * a));
        r.StrokeEllipse(c, R - 1.f, R - 1.f, b.color.WithAlpha((0.30f + 0.70f * hov) * a), 2.f);
        if (b.holdProgress > 0.001f) {
            r.Arc(c, R + 8.f, 0.f, 360.f * b.holdProgress, b.color.WithAlpha(a), 4.f);
        }
        if (b.flash.x > 0.001f) {
            float f = Clamp01(b.flash.x);
            r.StrokeEllipse(c, R * (1.f + 0.9f * f), R * (1.f + 0.9f * f), b.color.WithAlpha((1.f - f) * a), 3.f);
        }

        // Icon with its little personality.
        float iconSc = sc;
        float rot = 0.f;
        if (b.action == Action::Restart) rot = b.spin;
        if (b.action == Action::Shutdown) iconSc *= 1.f + 0.07f * hov * (0.5f + 0.5f * std::sin((float)now * 7.f));
        if (b.action == Action::Sleep) rot = -14.f * hov * std::sin((float)now * 3.f);
        if (b.action == Action::Lock) rot = 8.f * hov * std::sin((float)now * 18.f) * std::max(0.f, 1.f - (float)std::fmod(now, 1.4) * 2.5f);
        r.SetTransform(D2D1::Matrix3x2F::Scale(iconSc, iconSc, c) * D2D1::Matrix3x2F::Rotation(rot, c));
        r.Text(std::wstring(1, b.glyph), Font::IconHuge, { c.x - 32.f, c.y - 32.f, c.x + 32.f, c.y + 32.f },
               Color::Lerp(b.color, white, 0.18f * hov).WithAlpha(a), Align::Center);
        r.ResetTransform();

        float ly = Center(i).y + kRadius + 30.f + (1.f - std::min(1.f, ap)) * 28.f;
        r.Text(b.label, Font::Label, { c.x - 80.f, ly - 12.f, c.x + 80.f, ly + 12.f },
               th.text.WithAlpha(a * (0.62f + 0.38f * hov)), Align::Center);
        if (b.hold && hov > 0.01f) {
            const wchar_t* hint = b.holdProgress > 0.02f ? L"keep holding…" : L"press & hold";
            r.Text(hint, Font::Sub, { c.x - 80.f, ly + 9.f, c.x + 80.f, ly + 29.f }, b.color.WithAlpha(a * hov), Align::Center);
        }
    }

    r.Text(L"Esc to close   ·   ← → to choose   ·   Enter to select",
           Font::Sub, { 0, kPanelH - 38.f, wDip_, kPanelH - 16.f }, th.subtext.WithAlpha(headA * 0.85f), Align::Center);
    r.End();

    POINT src{ 0, 0 }, pos{ panelX_, panelY_ };
    SIZE size{ panelW_, panelH_ };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(panel_, screen, &pos, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

Action Overlay::Run() {
    if (!Create()) { Destroy(); return Action::None; }
    openAt_ = NowSeconds();
    double last = openAt_;
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
        float dt = (float)std::min(now - last, 1.0 / 30.0);
        last = now;
        Update(dt);
        Render();
        if (done_) break;
        if (Animating()) {
            // Pace frames to the display refresh, but no faster than ~80 fps on high-refresh screens.
            double frameStart = now;
            do {
                if (FAILED(DwmFlush())) { Sleep(8); break; }
            } while (NowSeconds() - frameStart < 0.012);
        } else {
            WaitMessage();
            last = NowSeconds();
        }
    }
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
        case WM_KEYDOWN: case WM_SYSKEYDOWN: o->Key(w, true, (l & (1 << 30)) != 0); return 0;
        case WM_KEYUP: case WM_SYSKEYUP: o->Key(w, false, false); return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) o->Close();  // Win key, Alt+Tab, another app: just go away
            break;
        case WM_CLOSE: o->Close(); return 0;
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
    switch (action) {
    case Action::Lock: LockWorkStation(); break;
    case Action::Sleep: EnableShutdownPrivilege(); SetSuspendState(FALSE, FALSE, FALSE); break;
    case Action::Hibernate: EnableShutdownPrivilege(); SetSuspendState(TRUE, FALSE, FALSE); break;
    case Action::DisplayOff: app.TurnDisplayOffSoon(); break;
    case Action::SignOut: ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); break;
    case Action::Restart: EnableShutdownPrivilege(); ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_FLAG_PLANNED); break;
    case Action::Shutdown: EnableShutdownPrivilege(); ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_FLAG_PLANNED); break;
    case Action::None: break;
    }
}
