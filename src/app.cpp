// App controller: owns the bars, timers, system notifications, fullscreen detection and menus.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include "app.h"
#include "power.h"
#include <shellscalingapi.h>
#include <wtsapi32.h>
#include <powrprof.h>
#include <psapi.h>
#include <dbt.h>

App* App::I = nullptr;

namespace {
enum : UINT_PTR {
    T_TICK = 1, T_SLOW, T_WATCH, T_WINLIST, T_DISPLAY, T_SHELLWAIT, T_PEEKEND, T_TRIM, T_NET, T_BTPRESENCE, T_RENDER, T_DISPLAYOFF
};

const wchar_t* kControllerClass = L"TopBarController";
const wchar_t* kBarClass = L"TopBarWnd";
const wchar_t* kTipClass = L"TopBarTip";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Where Task Manager's "Startup apps" stores its enable/disable switch for Run entries.
const wchar_t* kStartupApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

DWORD WindowsBuild() {
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    static DWORD build = [] {
        OSVERSIONINFOW v{ sizeof(v) };
        auto fn = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (fn && fn(&v) == 0) return v.dwBuildNumber;
        return (DWORD)0;
    }();
    return build;
}

// Dark context menus. uxtheme's SetPreferredAppMode (ordinal 135, Windows 10 1903+) is what
// Explorer and Notepad++ use. Purely cosmetic: if it is missing we keep the light menus.
void EnableDarkMenus() {
    if (WindowsBuild() < 18362) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppMode_t = int(WINAPI*)(int);
    using FlushMenuThemes_t = void(WINAPI*)();
    auto setMode = (SetPreferredAppMode_t)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemes_t)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2 /* ForceDark */);
    if (flush) flush();
}

void ShellOpen(const wchar_t* target, const wchar_t* args = nullptr) {
    ShellExecuteW(nullptr, L"open", target, args, nullptr, SW_SHOWNORMAL);
}

void SendKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

bool ParseHotkey(const std::wstring& text, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    for (auto& tok : Split(ToLower(text), L'+')) {
        if (tok == L"ctrl" || tok == L"control") mods |= MOD_CONTROL;
        else if (tok == L"alt") mods |= MOD_ALT;
        else if (tok == L"shift") mods |= MOD_SHIFT;
        else if (tok == L"win" || tok == L"windows") mods |= MOD_WIN;
        else if (tok.size() == 1 && iswalnum(tok[0])) vk = (UINT)towupper(tok[0]);
        else if (tok.size() >= 2 && tok[0] == L'f' && iswdigit(tok[1])) {
            int n = _wtoi(tok.c_str() + 1);
            if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
        } else if (tok == L"space") vk = VK_SPACE;
        else if (tok == L"`" || tok == L"backtick") vk = VK_OEM_3;
        else if (tok == L"pause") vk = VK_PAUSE;
        else if (tok == L"insert") vk = VK_INSERT;
        else if (tok == L"home") vk = VK_HOME;
        else if (tok == L"end") vk = VK_END;
        else return false;
    }
    return vk != 0;
}

int IsoWeek(const SYSTEMTIME& st, int* dayOfYear) {
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int y = st.wYear;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int doy = cum[st.wMonth - 1] + st.wDay + (leap && st.wMonth > 2 ? 1 : 0);
    if (dayOfYear) *dayOfYear = doy;
    int wday = st.wDayOfWeek == 0 ? 7 : st.wDayOfWeek;
    auto p = [](int yr) { return (yr + yr / 4 - yr / 100 + yr / 400) % 7; };
    auto weeks = [&](int yr) { return 52 + ((p(yr) == 4 || p(yr - 1) == 3) ? 1 : 0); };
    int w = (doy - wday + 10) / 7;
    if (w < 1) return weeks(y - 1);
    if (w > weeks(y)) return 1;
    return w;
}

void CALLBACK NetChangeCallback(PVOID ctx, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    PostMessageW((HWND)ctx, WM_APP_NETCHANGE, 0, 0);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    // Never leave reserved screen space behind if something goes wrong.
    if (App::I) {
        for (auto& b : App::I->bars) {
            if (b && b->hwnd) {
                APPBARDATA abd{ sizeof(abd) };
                abd.hWnd = b->hwnd;
                SHAppBarMessage(ABM_REMOVE, &abd);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

Mod ModFromName(const std::wstring& n) {
    if (n == L"windows" || n == L"taskbar") return Mod::Windows;
    if (n == L"clock") return Mod::Clock;
    if (n == L"netspeed") return Mod::NetSpeed;
    if (n == L"network" || n == L"wifi") return Mod::Network;
    if (n == L"cpu") return Mod::Cpu;
    if (n == L"gpu") return Mod::Gpu;
    if (n == L"memory" || n == L"ram") return Mod::Memory;
    if (n == L"disk") return Mod::Disk;
    if (n == L"volume" || n == L"audio") return Mod::Volume;
    if (n == L"bluetooth") return Mod::Bluetooth;
    if (n == L"battery") return Mod::Battery;
    if (n == L"power") return Mod::Power;
    return Mod::None;
}

// ---------------------------------------------------------------------------------------------

int App::Run(HINSTANCE instance, int command) {
    I = this;
    inst = instance;
    SetUnhandledExceptionFilter(CrashFilter);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    cfg.Load();

    if (cfg.efficiencyMode) {
        PROCESS_POWER_THROTTLING_STATE st{};
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    }
    EnableDarkMenus();

    if (!renderer.Init(cfg)) {
        MessageBoxW(nullptr, L"TopBar could not initialise Direct2D.", TOPBAR_NAME, MB_ICONERROR);
        return 1;
    }
    UpdateMetrics();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpfnWndProc = StaticProc;
    wc.lpszClassName = kControllerClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = Bar::StaticProc;
    wc.lpszClassName = kBarClass;
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, TOPBAR_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    tip.Create(inst);

    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    msgShellHook_ = RegisterWindowMessageW(L"SHELLHOOK");
    ChangeWindowMessageFilterEx(hwnd, msgTaskbarCreated_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_APP_COMMAND, MSGFLT_ALLOW, nullptr);

    stats.useUtility = cfg.cpuTaskManager;
    stats.Init();
    audio.Init(hwnd);

    RegisterShellHookWindow(hwnd);
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    hooks_.push_back(SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags));
    hooks_.push_back(SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, WinEventProc, 0, 0, flags));
    hooks_.push_back(SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0, 0, flags));
    hooks_.push_back(SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, WinEventProc, 0, 0, flags));

    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    powerNotify_[0] = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    powerNotify_[1] = RegisterPowerSettingNotification(hwnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    powerNotify_[2] = RegisterPowerSettingNotification(hwnd, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
    NotifyIpInterfaceChange(AF_UNSPEC, NetChangeCallback, hwnd, FALSE, &netNotify_);

    RegisterHotkey();
    SyncAutostart();
    if (command == (int)RemoteCommand::Toggle) userHidden_ = true;

    // At logon Explorer may not be ready to host appbars yet; wait for it.
    if (FindWindowW(L"Shell_TrayWnd", nullptr)) Start();
    else SetTimer(hwnd, T_SHELLWAIT, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Stop();
    for (auto h : hooks_) if (h) UnhookWinEvent(h);
    if (netNotify_) CancelMibChangeNotify2(netNotify_);
    for (auto p : powerNotify_) if (p) UnregisterPowerSettingNotification(p);
    audio.Shutdown();
    stats.Shutdown();
    renderer.Shutdown();
    CoUninitialize();
    return 0;
}

void App::Start() {
    if (started_) return;
    started_ = true;
    KillTimer(hwnd, T_SHELLWAIT);
    RebuildBars();
    windows.Refresh();
    windows.UpdateIcons(renderer);
    windowsDirty_ = false;
    Watch();
    ApplyVisibility();
    SetTimer(hwnd, T_TRIM, 8000, nullptr);
}

void App::Stop() {
    for (auto& b : bars) b->UnregisterAppBar();
    bars.clear();
    if (tip.hwnd) DestroyWindow(tip.hwnd);
    tip.hwnd = nullptr;
}

void App::UpdateMetrics() {
    wPct = renderer.Measure(L"100%", Font::Text);
    wTemp = renderer.Measure(L"100°", Font::Text);
    wRate = renderer.Measure(cfg.netBits ? L"99.9 Mb/s" : L"999 KB/s", Font::Text);
    wIconGap = 5.f;
}

void App::RegisterHotkey() {
    if (hotkeyOk_) UnregisterHotKey(hwnd, 1);
    hotkeyOk_ = false;
    UINT mods = 0, vk = 0;
    if (!cfg.peekHotkey.empty() && ParseHotkey(cfg.peekHotkey, mods, vk))
        hotkeyOk_ = RegisterHotKey(hwnd, 1, mods | MOD_NOREPEAT, vk) != FALSE;
}

void App::ReloadConfig() {
    cfg.Load();
    ApplyConfig();
}

void App::ApplyConfig() {
    SyncAutostart();
    stats.useUtility = cfg.cpuTaskManager;
    renderer.SetFonts(cfg);
    UpdateMetrics();
    RegisterHotkey();
    RebuildBars();
    for (auto& b : bars) b->Reposition();
    ApplyVisibility();
    RenderAll();
}

struct MonEnumCtx { std::vector<MonitorDesc> list; };

static BOOL CALLBACK MonEnumProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    MonitorDesc d;
    d.handle = hm;
    d.rect = mi.rcMonitor;
    d.device = mi.szDevice;
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy))) d.dpi = dx;
    ((MonEnumCtx*)lp)->list.push_back(d);
    return TRUE;
}

void App::RebuildBars() {
    MonEnumCtx ctx;
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&ctx);
    if (cfg.monitors == L"primary") {
        ctx.list.erase(std::remove_if(ctx.list.begin(), ctx.list.end(), [](const MonitorDesc& d) { return !d.primary; }),
                       ctx.list.end());
    }
    // Remove bars for monitors that went away.
    for (auto it = bars.begin(); it != bars.end();) {
        bool found = std::any_of(ctx.list.begin(), ctx.list.end(), [&](const MonitorDesc& d) { return d.device == (*it)->mon.device; });
        if (!found) {
            if (tip.Owner() == it->get()) tip.Hide(__LINE__);
            it = bars.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& d : ctx.list) {
        auto it = std::find_if(bars.begin(), bars.end(), [&](const std::unique_ptr<Bar>& b) { return b->mon.device == d.device; });
        if (it != bars.end()) {
            (*it)->SetMonitor(d);
        } else {
            auto bar = std::make_unique<Bar>(*this, d);
            if (bar->Create()) bars.push_back(std::move(bar));
        }
    }
    // Keep bars ordered left-to-right (primary first when equal).
    std::sort(bars.begin(), bars.end(), [](const std::unique_ptr<Bar>& a, const std::unique_ptr<Bar>& b) {
        return a->mon.rect.left < b->mon.rect.left;
    });
    windowsDirty_ = true;
    RequestWindowRefresh();
    ApplyVisibility();
}

bool App::PeekActive() const { return peekUntil_ > NowSeconds(); }

bool App::AnyBarVisible() const {
    for (auto& b : bars) if (b->visible) return true;
    return false;
}

UINT App::CurrentInterval() const {
    if (stats.battery.present && !stats.battery.onAC) return (UINT)cfg.intervalBatteryMs;
    return (UINT)cfg.intervalMs;
}

void App::ApplyVisibility() {
    bool peek = PeekActive();
    std::vector<bool> want(bars.size());
    bool any = false;
    for (size_t i = 0; i < bars.size(); ++i) {
        Bar& b = *bars[i];
        bool fsHide = cfg.hideInFullscreen && b.fullscreen;
        want[i] = !userHidden_ && !sessionPaused_ && (!fsHide || peek || b.revealed);
        any |= want[i];
        if (!userHidden_ && !b.appbarRegistered) { b.RegisterAppBar(); b.Reposition(); }
        if (userHidden_ && b.appbarRegistered) b.UnregisterAppBar();
    }
    SetSampling(any);
    for (size_t i = 0; i < bars.size(); ++i) bars[i]->SetVisible(want[i]);
    if (!any) tip.Hide(__LINE__);
}

void App::SetSampling(bool on) {
    if (on == sampling_) return;
    sampling_ = on;
    if (on) {
        stats.ResetBaselines();
        stats.SampleFast();
        stats.SampleBattery();
        audio.Refresh();
        if (windowsDirty_) {
            windows.Refresh();
            windows.UpdateIcons(renderer);
            windowsDirty_ = false;
        }
        ScheduleTick(300);  // quick first refresh so numbers are current right away
        SetCoalescableTimer(hwnd, T_SLOW, 2000, nullptr, 200);
    } else {
        KillTimer(hwnd, T_TICK);
        KillTimer(hwnd, T_SLOW);
    }
}

void App::ScheduleTick(UINT ms) {
    SetCoalescableTimer(hwnd, T_TICK, ms, nullptr, std::max<ULONG>(ms / 10, 15));
}

void App::RenderAll() {
    for (auto& b : bars) if (b->visible) b->Render();
}

extern bool g_debugLog;
void App::Tick() {
    ++tickCount_;
    double t0 = NowSeconds();
    stats.sampleAllGpus = cfg.gpuSelect != L"auto" || tip.HitMod() == Mod::Gpu;
    UpdateCpuCounterDemand();
    stats.SampleFast();
    double t1 = NowSeconds();
    RenderAll();
    double t2 = NowSeconds();
    tip.Refresh();
    if (g_debugLog) DebugLog(L"tick sample=%.2fms render=%.2fms", (t1 - t0) * 1000, (t2 - t1) * 1000);
    ScheduleTick(CurrentInterval());
}

void App::SlowTick() {
    ++slowCount_;
    if (cfg.ChangedOnDisk()) {
        ReloadConfig();
        return;
    }
    double t0 = NowSeconds();
    stats.SampleSlow();
    if (g_debugLog) DebugLog(L"slow sampleSlow=%.2fms events=%u shell=%u winrefresh=%u", (NowSeconds() - t0) * 1000, eventCount_, shellCount_, refreshCount_);
    if (slowCount_ % 3 == 0) {
        stats.SampleNetInfo();
        stats.SampleBluetooth();
    }
    if (slowCount_ % 15 == 1) stats.SampleDisks();
    if (slowCount_ % 15 == 0) stats.SampleBattery();
    RequestWindowRefresh();  // safety net for anything the shell hook missed
}

void App::RequestWindowRefresh() {
    windowsDirty_ = true;
    // Throttle: one pending refresh at a time (bursts of events collapse into one pass).
    if (sampling_ && !winlistPending_) {
        winlistPending_ = true;
        SetTimer(hwnd, T_WINLIST, 80, nullptr);
    }
}

void App::RequestRender() {
    if (sampling_ && !renderPending_) {
        renderPending_ = true;
        SetTimer(hwnd, T_RENDER, 150, nullptr);
    }
}

void App::RequestWatch() {
    if (started_ && !sessionPaused_) SetTimer(hwnd, T_WATCH, 40, nullptr);
}

// --- fullscreen detection --------------------------------------------------------------------

namespace {
bool IsShellClass(const wchar_t* cls) {
    static const wchar_t* names[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                      L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow",
                                      L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskListThumbnailWnd",
                                      L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland",
                                      L"Shell_InputSwitchTopLevelWindow", L"LockScreenControllerProxyWindow" };
    for (auto n : names) if (wcscmp(cls, n) == 0) return true;
    return false;
}

struct FsScan {
    struct Mon { RECT rc; int result = -1; };
    std::vector<Mon> mons;
    DWORD selfPid = 0;
    int unresolved = 0;
};

BOOL CALLBACK FsEnumProc(HWND h, LPARAM lp) {
    auto* s = (FsScan*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == s->selfPid) return TRUE;
    wchar_t cls[96];
    GetClassNameW(h, cls, _countof(cls));
    if (IsShellClass(cls)) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    // A normal maximized window is not "fullscreen" even if it pokes past the edges.
    bool maximizedFramed = IsZoomed(h) && (style & WS_CAPTION) == WS_CAPTION;
    for (auto& m : s->mons) {
        if (m.result != -1) continue;
        RECT inter;
        if (!IntersectRect(&inter, &r, &m.rc)) continue;
        bool covers = !maximizedFramed && r.left <= m.rc.left && r.top <= m.rc.top && r.right >= m.rc.right && r.bottom >= m.rc.bottom;
        if (covers) { m.result = 1; --s->unresolved; }
        else if (!(ex & WS_EX_TOPMOST)) { m.result = 0; --s->unresolved; }
        // Small always-on-top windows (PiP, widgets) don't decide; keep looking underneath.
    }
    return s->unresolved > 0;
}
}  // namespace

void App::DetectFullscreen() {
    FsScan scan;
    scan.selfPid = GetCurrentProcessId();
    for (auto& b : bars) scan.mons.push_back({ b->mon.rect, -1 });
    scan.unresolved = (int)scan.mons.size();
    if (scan.unresolved) EnumWindows(FsEnumProc, (LPARAM)&scan);
    for (size_t i = 0; i < bars.size(); ++i) bars[i]->fullscreen = scan.mons[i].result == 1;

    // User-listed games hide the bar on their monitor while focused, even windowed.
    if (!cfg.gameApps.empty()) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            std::wstring exe = ProcessImageName(pid);
            if (std::find(cfg.gameApps.begin(), cfg.gameApps.end(), exe) != cfg.gameApps.end()) {
                HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
                for (auto& b : bars) if (b->mon.handle == hm) b->fullscreen = true;
            }
        }
    }
}

void App::Watch() {
    if (!started_ || sessionPaused_) return;
    double tw = NowSeconds();
    DetectFullscreen();
    if (g_debugLog) DebugLog(L"watch detect=%.2fms", (NowSeconds() - tw) * 1000);

    UINT next = 1000;
    bool anyHidden = false;
    if (cfg.hideInFullscreen && !userHidden_) {
        POINT pt{};
        GetCursorPos(&pt);
        CURSORINFO ci{ sizeof(ci) };
        bool cursorShown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
        RECT clip{}, virt{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
        virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        GetClipCursor(&clip);
        bool clipped = !EqualRect(&clip, &virt);
        double now = NowSeconds();

        for (auto& bp : bars) {
            Bar& b = *bp;
            if (!b.fullscreen) { b.revealed = false; b.dwellStart = b.leaveStart = 0; continue; }
            anyHidden = true;
            if (!cfg.revealOnHover) continue;
            // Games capture or hide the mouse: never pop up over them.
            bool usable = cursorShown && !clipped;
            if (usable) {
                QUERY_USER_NOTIFICATION_STATE q;
                if (SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN) usable = false;
            }
            bool inX = pt.x >= b.mon.rect.left && pt.x < b.mon.rect.right;
            if (!b.revealed) {
                bool atTop = inX && pt.y <= b.mon.rect.top + 1 && pt.y >= b.mon.rect.top - 2;
                if (usable && atTop) {
                    if (b.dwellStart == 0) b.dwellStart = now;
                    if ((now - b.dwellStart) * 1000.0 >= cfg.revealDelayMs) { b.revealed = true; b.leaveStart = 0; }
                } else {
                    b.dwellStart = 0;
                }
                if (usable) next = std::min<UINT>(next, 100);
                else next = std::min<UINT>(next, 500);
            } else {
                bool over = inX && pt.y >= b.mon.rect.top - 2 && pt.y < b.mon.rect.top + b.pxHeight + (int)(8 * b.Scale());
                over = over || tip.Visible();
                if (over) b.leaveStart = 0;
                else if (b.leaveStart == 0) b.leaveStart = now;
                else if (now - b.leaveStart > 0.45) { b.revealed = false; b.dwellStart = 0; }
                next = std::min<UINT>(next, 100);
            }
        }
    }
    (void)anyHidden;
    ApplyVisibility();
    SetCoalescableTimer(hwnd, T_WATCH, next, nullptr, next / 5);
}

// --- window procedure ------------------------------------------------------------------------

LRESULT CALLBACK App::StaticProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (I && I->hwnd == h) return I->Proc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

void CALLBACK App::WinEventProc(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (!I || obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    if ((ev == EVENT_OBJECT_CLOAKED || ev == EVENT_OBJECT_UNCLOAKED) && GetAncestor(h, GA_ROOT) != h) return;
    ++I->eventCount_;
    I->RequestWindowRefresh();
    if (ev == EVENT_SYSTEM_FOREGROUND || ev == EVENT_SYSTEM_MOVESIZEEND || ev == EVENT_SYSTEM_MINIMIZESTART ||
        ev == EVENT_SYSTEM_MINIMIZEEND)
        I->RequestWatch();
}

LRESULT App::Proc(UINT m, WPARAM w, LPARAM l) {
    if (m == msgShellHook_ && msgShellHook_) {
        ++shellCount_;
        switch (w & 0x7FFF) {
        case HSHELL_REDRAW:
            if (w == HSHELL_FLASH) {
                windows.SetFlashing((HWND)l, true);
                RequestRender();
            } else if (windows.Find((HWND)l)) {
                // Just a title change (some apps animate their title): update it in place and
                // only redraw if that title is actually on screen.
                if (windows.UpdateTitle((HWND)l) && (HWND)l == windows.Active() && cfg.showActiveTitle) RequestRender();
            } else {
                RequestWindowRefresh();
            }
            return 0;
        case HSHELL_WINDOWREPLACED:
            windows.MarkIconDirty((HWND)l);
            break;
        }
        RequestWindowRefresh();
        if ((w & 0x7FFF) == HSHELL_WINDOWACTIVATED) RequestWatch();
        return 0;
    }
    if (m == msgTaskbarCreated_ && msgTaskbarCreated_) {
        // Explorer restarted (e.g. after an update): register our screen space again.
        if (!started_) { Start(); return 0; }
        for (auto& b : bars) { b->appbarRegistered = false; }
        ApplyVisibility();
        for (auto& b : bars) b->Reposition();
        return 0;
    }

    switch (m) {
    case WM_TIMER:
        switch (w) {
        case T_TICK: Tick(); break;
        case T_SLOW: SlowTick(); break;
        case T_WATCH: Watch(); break;
        case T_DISPLAYOFF:
            KillTimer(hwnd, T_DISPLAYOFF);
            DefWindowProcW(hwnd, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
            break;
        case T_RENDER:
            KillTimer(hwnd, T_RENDER);
            renderPending_ = false;
            RenderAll();
            break;
        case T_WINLIST:
            KillTimer(hwnd, T_WINLIST);
            winlistPending_ = false;
            ++refreshCount_;
            if (sampling_) {
                bool changed = windows.Refresh();
                windows.UpdateIcons(renderer);
                windowsDirty_ = false;
                if (changed) RenderAll();
            }
            break;
        case T_DISPLAY:
            KillTimer(hwnd, T_DISPLAY);
            stats.RefreshGpuOutputs();
            RebuildBars();
            Watch();
            break;
        case T_SHELLWAIT:
            if (FindWindowW(L"Shell_TrayWnd", nullptr)) Start();
            break;
        case T_PEEKEND: {
            KillTimer(hwnd, T_PEEKEND);
            // Stay while the mouse is on the bar.
            POINT pt;
            GetCursorPos(&pt);
            for (auto& b : bars) {
                if (b->visible && PtInRect(&b->barRect, pt)) { b->revealed = true; b->leaveStart = 0; }
            }
            peekUntil_ = 0;
            ApplyVisibility();
            break;
        }
        case T_TRIM:
            KillTimer(hwnd, T_TRIM);
            // Hand back start-up memory (fonts, COM init) we will not touch again.
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            break;
        case T_NET:
            KillTimer(hwnd, T_NET);
            stats.SampleNetInfo();
            if (sampling_) RenderAll();
            break;
        case T_BTPRESENCE:
            KillTimer(hwnd, T_BTPRESENCE);
            stats.RefreshBluetoothPresence();
            stats.SampleBluetooth();
            if (sampling_) RenderAll();
            break;
        }
        return 0;

    case WM_HOTKEY: {
        bool anyFsHidden = false;
        for (auto& b : bars) if (b->fullscreen && cfg.hideInFullscreen) anyFsHidden = true;
        if (anyFsHidden && !userHidden_) {
            if (PeekActive()) {
                peekUntil_ = 0;
                for (auto& b : bars) b->revealed = false;
            } else {
                peekUntil_ = NowSeconds() + cfg.peekSeconds;
                SetTimer(hwnd, T_PEEKEND, (UINT)cfg.peekSeconds * 1000, nullptr);
            }
        } else {
            userHidden_ = !userHidden_;
            peekUntil_ = 0;
        }
        ApplyVisibility();
        return 0;
    }

    case WM_APP_AUDIO:
        audio.Refresh();
        if (sampling_) RenderAll();
        tip.Refresh();
        return 0;
    case WM_APP_AUDIO_DEVICE:
        audio.Reinit();
        if (sampling_) RenderAll();
        return 0;
    case WM_APP_NETCHANGE:
        SetTimer(hwnd, T_NET, 700, nullptr);
        return 0;
    case WM_APP_COMMAND:
        switch ((RemoteCommand)w) {
        case RemoteCommand::Exit: DestroyWindow(hwnd); break;
        case RemoteCommand::Toggle: userHidden_ = !userHidden_; ApplyVisibility(); break;
        case RemoteCommand::Reload: ReloadConfig(); break;
        }
        return 0;

    case WM_DISPLAYCHANGE:
        SetTimer(hwnd, T_DISPLAY, 500, nullptr);
        return 0;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t*)l, L"intl") == 0) RenderAll();
        break;
    case WM_DEVICECHANGE:
        if (w == DBT_DEVNODES_CHANGED) SetTimer(hwnd, T_BTPRESENCE, 1500, nullptr);
        return TRUE;

    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK || w == WTS_CONSOLE_DISCONNECT) {
            sessionPaused_ = true;
            KillTimer(hwnd, T_WATCH);
            ApplyVisibility();
        } else if (w == WTS_SESSION_UNLOCK || w == WTS_CONSOLE_CONNECT) {
            sessionPaused_ = false;
            Watch();
        }
        return 0;

    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            auto* ps = (POWERBROADCAST_SETTING*)l;
            if (ps->PowerSetting == GUID_CONSOLE_DISPLAY_STATE && ps->DataLength >= sizeof(DWORD)) {
                DWORD state = *(DWORD*)ps->Data;  // 0 off, 1 on, 2 dimmed
                bool off = state == 0;
                if (off != sessionPaused_) {
                    sessionPaused_ = off;
                    if (off) { KillTimer(hwnd, T_WATCH); ApplyVisibility(); }
                    else Watch();
                }
            } else {
                stats.SampleBattery();
                if (sampling_) { RenderAll(); ScheduleTick(CurrentInterval()); }
            }
        } else if (w == PBT_APMPOWERSTATUSCHANGE) {
            stats.SampleBattery();
            if (sampling_) RenderAll();
        } else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            stats.ResetBaselines();
            SetTimer(hwnd, T_DISPLAY, 1000, nullptr);
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w) for (auto& b : bars) b->UnregisterAppBar();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// --- tooltips --------------------------------------------------------------------------------

void App::TrimMemorySoon() {
    SetTimer(hwnd, T_TRIM, 3000, nullptr);
}

void App::UpdateCpuCounterDemand() {
    bool want = tip.HitMod() == Mod::Cpu;
    for (auto& b : bars)
        if (b->hover >= 0 && b->hover < (int)b->hits.size() && b->hits[b->hover].mod == Mod::Cpu) want = true;
    stats.SetCpuCountersActive(want);
}

void App::TurnDisplayOffSoon() {
    // A short delay so the click that chose it doesn't wake the screen straight back up.
    SetTimer(hwnd, T_DISPLAYOFF, 700, nullptr);
}

void App::BuildTooltip(const Hit& hit, std::wstring& title, std::vector<TipLine>& lines, TipExtra& extra) {
    const Stats& s = stats;
    auto pct = [](float v) { return Fmt(L"%.0f%%", v * 100.f); };
    switch (hit.mod) {
    case Mod::Windows: {
        WinItem* w = windows.Find(hit.hwnd);
        if (!w) break;
        title = w->title;
        lines.push_back({ L"App", w->exe });
        lines.push_back({ L"State", w->minimized ? L"Minimized" : (hit.hwnd == windows.Active() ? L"Active" : L"Open") });
        lines.push_back({ L"", L"Click switch · Middle-click close · Right-click menu" });
        break;
    }
    case Mod::WindowOverflow:
        title = L"More windows";
        lines.push_back({ L"", L"Click to pick a window" });
        break;
    case Mod::Clock: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t buf[128];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, buf, _countof(buf), nullptr);
        title = buf;
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, buf, _countof(buf));
        lines.push_back({ L"Time", buf });
        int doy = 0;
        int week = IsoWeek(st, &doy);
        lines.push_back({ L"Week", Fmt(L"%d", week) });
        lines.push_back({ L"Day of year", Fmt(L"%d", doy) });
        lines.push_back({ L"Uptime", FormatDuration(GetTickCount64() / 1000) });
        lines.push_back({ L"", L"Click for calendar & notifications" });
        break;
    }
    case Mod::NetSpeed:
        title = L"Network traffic";
        lines.push_back({ L"Download", FormatRate(s.rxRate, cfg.netBits) });
        lines.push_back({ L"Upload", FormatRate(s.txRate, cfg.netBits) });
        lines.push_back({ L"Received since boot", FormatBytes(s.rxTotal) });
        lines.push_back({ L"Sent since boot", FormatBytes(s.txTotal) });
        break;
    case Mod::Network: {
        const NetInfo& n = s.net;
        title = n.kind == NetInfo::Wifi ? L"Wi-Fi" : n.kind == NetInfo::Ethernet ? L"Ethernet"
              : n.kind == NetInfo::Other ? n.adapterName : L"Not connected";
        if (n.kind != NetInfo::None) {
            if (!n.description.empty()) lines.push_back({ L"Adapter", n.description });
            if (!n.ipv4.empty()) lines.push_back({ L"IPv4", n.ipv4 });
            if (n.linkSpeed) {
                double mbps = n.linkSpeed / 1e6;
                lines.push_back({ L"Link speed", mbps >= 1000 ? Fmt(L"%.1f Gbps", mbps / 1000) : Fmt(L"%.0f Mbps", mbps) });
            }
            lines.push_back({ L"Internet", n.internet ? L"Connected" : L"No internet access" });
        }
        if (n.wifiPresent) {
            std::wstring wifi = !n.wifiRadioOn ? L"Off" : n.wifiConnected
                ? (n.ssid.empty() ? L"Connected" : n.ssid) + (n.signal >= 0 ? Fmt(L"  (%d%%)", n.signal) : L"")
                : L"On, not connected";
            lines.push_back({ L"Wi-Fi", wifi });
        }
        lines.push_back({ L"", L"Click for networks · Right-click settings" });
        break;
    }
    case Mod::Cpu: {
        title = s.cpuName;
        lines.push_back({ L"Usage", pct(s.cpu) + (s.useUtility ? L"  (Task Manager method)" : L"") });
        if (s.utilityAvailable) {
            if (!s.cpuCountersFresh) {
                lines.push_back({ L"Clock speed", L"measuring\u2026" });
            } else {
                if (s.cpuMHz > 0)
                    lines.push_back({ L"Clock speed", Fmt(L"%.2f GHz  (base %.2f GHz)", s.cpuMHz / 1000.f, s.cpuBaseMHz / 1000.f) });
                if (s.useUtility) lines.push_back({ L"Busy time", pct(s.cpuBusy) });
                else lines.push_back({ L"Task Manager", pct(s.cpuUtility) + L"  (busy time \u00D7 clock / base clock)" });
            }
        }
        if (s.physicalCores) lines.push_back({ L"Cores", Fmt(L"%d cores, %d threads", s.physicalCores, s.logicalCpus) });
        extra.cpuHistory = s.cpuHistory.size() >= 2;
        extra.coreGrid = !s.cores.empty();
        if (!std::isnan(s.cpuTemp))
            lines.push_back({ L"Temperature", Fmt(L"%.0f °C  (peak %.0f °C)", s.cpuTemp, s.cpuPeakTemp) });
        PERFORMANCE_INFORMATION pi{ sizeof(pi) };
        if (GetPerformanceInfo(&pi, sizeof(pi)))
            lines.push_back({ L"Processes", Fmt(L"%lu  (%lu threads)", pi.ProcessCount, pi.ThreadCount) });
        lines.push_back({ L"Uptime", FormatDuration(GetTickCount64() / 1000) });
        lines.push_back({ L"", L"Click for Task Manager" });
        break;
    }
    case Mod::Gpu: {
        if (s.gpus.empty()) break;
        int idx = s.mainGpu >= 0 ? s.mainGpu : 0;
        const GpuStat& g = s.gpus[idx];
        title = g.name;
        lines.push_back({ L"Usage", pct(g.usage) });
        if (!std::isnan(g.temp)) lines.push_back({ L"Temperature", Fmt(L"%.0f °C", g.temp) });
        else if (g.hybridDiscrete && !g.hasOutputs && g.idleSamples >= 3) lines.push_back({ L"State", L"Idle (powered down)" });
        if (g.dedicatedTotal)
            lines.push_back({ L"Dedicated memory", FormatBytes(g.dedicatedUsed) + L" / " + FormatBytes(g.dedicatedTotal) });
        lines.push_back({ L"Shared memory", FormatBytes(g.sharedUsed) });
        for (size_t i = 0; i < s.gpus.size(); ++i) {
            if ((int)i == idx) continue;
            const GpuStat& o = s.gpus[i];
            std::wstring v = pct(o.usage);
            if (!std::isnan(o.temp)) v += Fmt(L"  %.0f °C", o.temp);
            lines.push_back({ o.name, v });
        }
        lines.push_back({ L"", L"Click for Task Manager" });
        break;
    }
    case Mod::Memory:
        title = L"Memory";
        lines.push_back({ L"In use", FormatBytes(s.memUsed) + L" / " + FormatBytes(s.memTotal) + L"  (" + pct(s.memLoad) + L")" });
        lines.push_back({ L"Available", FormatBytes(s.memTotal - s.memUsed) });
        lines.push_back({ L"Committed", FormatBytes(s.commitTotal) + L" / " + FormatBytes(s.commitLimit) });
        lines.push_back({ L"", L"Click for Task Manager" });
        break;
    case Mod::Disk:
        title = L"Storage";
        for (auto& d : s.drives) {
            std::wstring label = Fmt(L"%c:", d.letter) + (d.label.empty() ? L"" : L" " + d.label);
            lines.push_back({ label, FormatBytes(d.free) + L" free of " + FormatBytes(d.total) });
        }
        lines.push_back({ L"", L"Click to open This PC" });
        break;
    case Mod::Volume:
        title = audio.deviceName.empty() ? L"Sound" : audio.deviceName;
        lines.push_back({ L"Volume", audio.muted ? L"Muted" : Fmt(L"%.0f%%", audio.level * 100.f) });
        lines.push_back({ L"", L"Click mute · Scroll volume · Right-click mixer" });
        break;
    case Mod::Bluetooth:
        title = L"Bluetooth";
        lines.push_back({ L"Status", s.bt.on ? L"On" : L"Off" });
        if (!s.bt.radioName.empty()) lines.push_back({ L"Adapter", s.bt.radioName });
        if (s.bt.connected.empty()) lines.push_back({ L"Connected", s.bt.on ? L"No devices" : L"—" });
        for (size_t i = 0; i < s.bt.connected.size(); ++i) lines.push_back({ i == 0 ? L"Connected" : L"", s.bt.connected[i] });
        lines.push_back({ L"", L"Click for Bluetooth settings" });
        break;
    case Mod::Battery: {
        const BatteryInfo& b = s.battery;
        title = L"Battery";
        lines.push_back({ L"Charge", Fmt(L"%d%%", b.percent) });
        lines.push_back({ L"Status", b.charging ? L"Charging" : b.onAC ? L"Plugged in" : L"On battery" });
        if (!b.onAC && b.secondsLeft > 0) lines.push_back({ L"Time left", FormatDuration((uint64_t)b.secondsLeft) });
        lines.push_back({ L"Energy saver", b.saver ? L"On" : L"Off" });
        lines.push_back({ L"", L"Click for power settings" });
        break;
    }
    case Mod::Power:
        title = L"Power";
        lines.push_back({ L"", L"Click for the power menu \u00B7 Right-click for a quick list" });
        break;
    default:
        break;
    }
}

// --- actions & menus -------------------------------------------------------------------------

UINT App::TrackMenu(HMENU menu, POINT pt) {
    HWND prev = GetForegroundWindow();
    SetForegroundWindow(hwnd);
    tip.Hide(__LINE__);
    UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
                                      pt.x, pt.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (!cmd && prev && IsWindow(prev)) SetForegroundWindow(prev);
    return cmd;
}

void App::OnModuleClick(Bar& bar, const Hit& hit, int button, POINT pt) {
    switch (hit.mod) {
    case Mod::Windows:
        if (button == 0) ActivateOrMinimize(hit.hwnd);
        else if (button == 2) PostMessageW(hit.hwnd, WM_CLOSE, 0, 0);
        else WindowMenu(hit.hwnd, pt);
        RequestWindowRefresh();
        return;
    case Mod::WindowOverflow:
        OverflowMenu(bar, pt);
        return;
    case Mod::Clock:
        if (button == 0) {
            if (WindowsBuild() >= 22000) SendKeys({ VK_LWIN, 'N' });
            else SendKeys({ VK_LWIN, VK_MENU, 'D' });
            return;
        }
        if (button == 1) { ShellOpen(L"ms-settings:dateandtime"); return; }
        break;
    case Mod::NetSpeed:
    case Mod::Network:
        if (button == 0) { ShellOpen(L"ms-availablenetworks:"); return; }
        if (button == 1) { ShellOpen(L"ms-settings:network-status"); return; }
        break;
    case Mod::Cpu:
    case Mod::Gpu:
    case Mod::Memory:
        if (button == 0) { ShellOpen(L"taskmgr.exe"); return; }
        break;
    case Mod::Disk:
        if (button == 0) { ShellOpen(L"explorer.exe", L"shell:MyComputerFolder"); return; }
        if (button == 1) { ShellOpen(L"ms-settings:storagesense"); return; }
        break;
    case Mod::Volume:
        if (button == 0) { audio.ToggleMute(); RenderAll(); tip.Refresh(); return; }
        if (button == 1) { ShellOpen(L"ms-settings:apps-volume"); return; }
        break;
    case Mod::Bluetooth:
        if (button == 0) { ShellOpen(L"ms-settings:bluetooth"); return; }
        break;
    case Mod::Battery:
        if (button == 0) { ShellOpen(L"ms-settings:batterysaver"); return; }
        break;
    case Mod::Power:
        if (button == 0) {
            MonitorDesc mon = bar.mon;  // copy: bars may be rebuilt while the menu is open
            tip.Hide(__LINE__);
            PowerOverlay::Run(*this, mon);
            return;
        }
        if (button == 1) { PowerMenu(pt); return; }
        break;
    default:
        break;
    }
    if (button == 1) ShowContextMenu(pt);
}

void App::OnModuleWheel(const Hit& hit, int delta) {
    if (hit.mod == Mod::Volume && audio.present) {
        float step = 0.02f * (delta / (float)WHEEL_DELTA);
        audio.SetLevel(std::round((audio.level + step) * 50.f) / 50.f);
        RenderAll();
        tip.Refresh();
    }
}

void App::PowerMenu(POINT pt) {
    enum { LOCK = 1, SLEEP, HIBERNATE, SIGNOUT, RESTART, SHUTDOWN };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, LOCK, L"Lock");
    AppendMenuW(m, MF_STRING, SLEEP, L"Sleep");
    if (IsPwrHibernateAllowed()) AppendMenuW(m, MF_STRING, HIBERNATE, L"Hibernate");
    AppendMenuW(m, MF_STRING, SIGNOUT, L"Sign out");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, RESTART, L"Restart");
    AppendMenuW(m, MF_STRING, SHUTDOWN, L"Shut down");
    UINT cmd = TrackMenu(m, pt);
    DestroyMenu(m);
    switch (cmd) {
    case LOCK: LockWorkStation(); break;
    case SLEEP: SleepPC(*this); break;
    case HIBERNATE: EnableShutdownPrivilege(); SetSuspendState(TRUE, FALSE, FALSE); break;
    case SIGNOUT: ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); break;
    case RESTART: EnableShutdownPrivilege(); ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_FLAG_PLANNED); break;
    case SHUTDOWN: EnableShutdownPrivilege(); ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_FLAG_PLANNED); break;
    }
}

void App::WindowMenu(HWND target, POINT pt) {
    enum { RESTORE = 1, MINIMIZE, MAXIMIZE, CLOSE };
    bool mini = IsIconic(target) != FALSE, maxi = IsZoomed(target) != FALSE;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | ((mini || maxi) ? 0 : MF_GRAYED), RESTORE, L"Restore");
    AppendMenuW(m, MF_STRING | (mini ? MF_GRAYED : 0), MINIMIZE, L"Minimize");
    AppendMenuW(m, MF_STRING | (maxi ? MF_GRAYED : 0), MAXIMIZE, L"Maximize");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CLOSE, L"Close window");
    UINT cmd = TrackMenu(m, pt);
    DestroyMenu(m);
    switch (cmd) {
    case RESTORE: ShowWindowAsync(target, SW_RESTORE); SetForegroundWindow(target); break;
    case MINIMIZE: ShowWindowAsync(target, SW_MINIMIZE); break;
    case MAXIMIZE: ShowWindowAsync(target, SW_MAXIMIZE); SetForegroundWindow(target); break;
    case CLOSE: PostMessageW(target, WM_CLOSE, 0, 0); break;
    }
}

void App::OverflowMenu(Bar& bar, POINT pt) {
    HMENU m = CreatePopupMenu();
    std::vector<HWND> targets;
    bool started = false;
    // Windows not shown on the bar (those after the last visible item).
    std::vector<HWND> shown;
    for (auto& h : bar.hits) if (h.mod == Mod::Windows) shown.push_back(h.hwnd);
    for (auto& w : windows.items) {
        if (std::find(shown.begin(), shown.end(), w.hwnd) != shown.end()) continue;
        if (cfg.windowsPerMonitor && bars.size() > 1 && w.monitor != bar.mon.handle) continue;
        std::wstring t = w.title.size() > 60 ? w.title.substr(0, 57) + L"..." : w.title;
        targets.push_back(w.hwnd);
        AppendMenuW(m, MF_STRING, targets.size(), t.c_str());
        started = true;
    }
    if (!started) { DestroyMenu(m); return; }
    UINT cmd = TrackMenu(m, pt);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= targets.size()) ActivateOrMinimize(targets[cmd - 1]);
}

bool App::AutostartDisabledByUser() {
    BYTE data[16] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kStartupApprovedKey, TOPBAR_NAME, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS)
        return false;
    return size > 0 && (data[0] & 1) != 0;  // 02 = enabled, 03 = disabled in Task Manager
}

void App::ClearAutostartDisabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, TOPBAR_NAME);  // no entry = enabled
        RegCloseKey(k);
    }
}

bool App::AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, TOPBAR_NAME, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return false;
    return !AutostartDisabledByUser();
}

// The settings file is the source of truth. On every launch we make sure the startup entry exists
// (and points at this exe, in case the folder moved) or is gone. A switch-off made in Task Manager's
// Startup apps page is left alone.
void App::SyncAutostart() {
#ifdef TOPBAR_DRY_RUN
    return;  // test builds run from elsewhere and must not take over the real install's autostart
#endif
    std::wstring want = L"\"" + ExePath() + L"\"";
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD size = sizeof(buf);
    bool exists = RegGetValueW(HKEY_CURRENT_USER, kRunKey, TOPBAR_NAME, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
    if (cfg.startWithWindows) {
        if (!exists || _wcsicmp(buf, want.c_str()) != 0) SetAutostart(true);
    } else if (exists) {
        SetAutostart(false);
    }
}

void App::SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, TOPBAR_NAME, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, TOPBAR_NAME);
    }
    RegCloseKey(k);
}

void App::ShowContextMenu(POINT pt) {
    enum { HIDE = 1, FULLSCREEN, REVEAL, AUTOSTART, EDIT, RELOAD, TASKMGR, EXIT };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"TopBar " TOPBAR_VERSION);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring hide = L"Hide bar";
    if (hotkeyOk_) hide += L"\t" + cfg.peekHotkey;
    AppendMenuW(m, MF_STRING, HIDE, hide.c_str());
    AppendMenuW(m, MF_STRING | (cfg.hideInFullscreen ? MF_CHECKED : 0), FULLSCREEN, L"Hide in fullscreen games && videos");
    AppendMenuW(m, MF_STRING | (cfg.revealOnHover ? MF_CHECKED : 0) | (cfg.hideInFullscreen ? 0 : MF_GRAYED), REVEAL,
                L"Peek from the top edge in fullscreen");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, EDIT, L"Edit settings...");
    AppendMenuW(m, MF_STRING, RELOAD, L"Reload settings");
    AppendMenuW(m, MF_STRING, TASKMGR, L"Task Manager");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, EXIT, L"Exit TopBar");
    UINT cmd = TrackMenu(m, pt);
    DestroyMenu(m);
    switch (cmd) {
    case HIDE: userHidden_ = true; ApplyVisibility(); break;
    case FULLSCREEN:
        cfg.SetValue(L"behavior", L"hide_in_fullscreen", cfg.hideInFullscreen ? L"0" : L"1");
        ReloadConfig();
        Watch();
        break;
    case REVEAL:
        cfg.SetValue(L"behavior", L"reveal_on_hover", cfg.revealOnHover ? L"0" : L"1");
        ReloadConfig();
        break;
    case AUTOSTART: {
        bool on = !AutostartEnabled();
        if (on) ClearAutostartDisabled();  // an explicit tick also undoes a Task Manager switch-off
        cfg.SetValue(L"behavior", L"start_with_windows", on ? L"1" : L"0");
        ReloadConfig();  // re-syncs the startup entry
        break;
    }
    case EDIT:
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", cfg.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + cfg.path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case RELOAD: ReloadConfig(); break;
    case TASKMGR: ShellOpen(L"taskmgr.exe"); break;
    case EXIT: DestroyWindow(hwnd); break;
    }
}
