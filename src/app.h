#pragma once
#include "common.h"
#include "config.h"
#include "render.h"
#include "stats.h"
#include "audio.h"
#include "winlist.h"

enum class Mod { None, Windows, WindowOverflow, Clock, NetSpeed, Network, Cpu, Gpu, Memory, Disk, Volume, Bluetooth, Battery, Power };

Mod ModFromName(const std::wstring& name);

struct Hit {
    D2D1_RECT_F rect{};
    Mod mod = Mod::None;
    HWND hwnd = nullptr;  // window items
};

struct MonitorDesc {
    HMONITOR handle = nullptr;
    RECT rect{};
    std::wstring device;
    bool primary = false;
    UINT dpi = 96;
};

struct TipLine { std::wstring label, value; };
struct TipExtra {
    bool cpuHistory = false;   // 60 s usage graph
    bool coreGrid = false;     // live per-core / per-thread bars
};

class App;

class Bar {
public:
    Bar(App& app, const MonitorDesc& m);
    ~Bar();
    bool Create();
    void SetMonitor(const MonitorDesc& m);
    void RegisterAppBar();
    void UnregisterAppBar();
    void Reposition();
    void SetVisible(bool show);
    void Render();
    void HideTip(int line = 0);

    HWND hwnd = nullptr;
    MonitorDesc mon;
    bool appbarRegistered = false;
    bool visible = false;
    bool fullscreen = false;     // a fullscreen window covers this monitor
    bool revealed = false;       // peeking via the top-edge hover
    double dwellStart = 0, leaveStart = 0;
    int pxWidth = 0, pxHeight = 0;
    RECT barRect{};              // screen pixels

    float Scale() const { return mon.dpi / 96.f; }
    std::vector<Hit> hits;
    int hover = -1;

    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    void EnsureSurface();
    void FreeSurface();
    int HitTest(int px, int py) const;
    void OnClick(int idx, int button, POINT screenPt);

    App& app_;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int surfW_ = 0, surfH_ = 0;
    bool tracking_ = false;
};

class Tooltip {
public:
    bool Create(HINSTANCE inst);
    void Show(Bar* bar, const Hit& hit);
    void Refresh();               // re-render with fresh values if visible
    void Hide(int line = 0);
    bool Visible() const { return visible_; }
    Bar* Owner() const { return owner_; }
    Mod HitMod() const { return visible_ ? hit_.mod : Mod::None; }
    HWND hwnd = nullptr;

private:
    void Draw();
    Bar* owner_ = nullptr;
    Hit hit_;
    bool visible_ = false;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
};

class App {
public:
    int Run(HINSTANCE inst, int command);
    static App* I;

    Config cfg;
    Renderer renderer;
    Stats stats;
    Audio audio;
    WindowList windows;
    Tooltip tip;
    std::vector<std::unique_ptr<Bar>> bars;
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;          // hidden controller window

    // Cached text metrics (DIPs) for stable widths.
    float wPct = 0, wTemp = 0, wRate = 0, wIconGap = 0;

    void RenderAll();
    void RequestWindowRefresh();
    void RequestRender();
    void RequestWatch();
    void ApplyVisibility();
    bool PeekActive() const;

    // Module content & actions (used by bars/tooltips)
    void BuildTooltip(const Hit& hit, std::wstring& title, std::vector<TipLine>& lines, TipExtra& extra);
    void TurnDisplayOffSoon();
    void UpdateCpuCounterDemand();
    void OnModuleClick(Bar& bar, const Hit& hit, int button, POINT pt);
    void OnModuleWheel(const Hit& hit, int delta);
    void ShowContextMenu(POINT pt);

private:
    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);

    void Start();
    void Stop();
    void ReloadConfig();
    void ApplyConfig();
    void RebuildBars();
    void Tick();
    void SlowTick();
    void Watch();
    void DetectFullscreen();
    void SetSampling(bool on);
    void ScheduleTick(UINT ms);
    void RegisterHotkey();
    void UpdateMetrics();
    void PowerMenu(POINT pt);
    void WindowMenu(HWND target, POINT pt);
    void OverflowMenu(Bar& bar, POINT pt);
    UINT TrackMenu(HMENU menu, POINT pt);
    bool AutostartEnabled();
    void SetAutostart(bool on);
    bool AnyBarVisible() const;
    UINT CurrentInterval() const;

    UINT msgTaskbarCreated_ = 0;
    UINT msgShellHook_ = 0;
    std::vector<HWINEVENTHOOK> hooks_;
    HPOWERNOTIFY powerNotify_[3] = {};
    HANDLE netNotify_ = nullptr;
    bool started_ = false;
    bool userHidden_ = false;
    bool sessionPaused_ = false;   // locked or display off
    bool sampling_ = false;
    bool windowsDirty_ = true;
    double peekUntil_ = 0;
    unsigned tickCount_ = 0;
    unsigned slowCount_ = 0;
    bool hotkeyOk_ = false;
    bool winlistPending_ = false;
public:
    unsigned eventCount_ = 0, shellCount_ = 0, refreshCount_ = 0;
private:
    bool renderPending_ = false;
};
