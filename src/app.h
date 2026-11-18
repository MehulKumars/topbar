#pragma once
#include "common.h"
#include "config.h"
#include "render.h"
#include "stats.h"
#include "audio.h"
#include "winlist.h"
#include "pet.h"

enum class Mod { None, Windows, WindowOverflow, Clock, NetSpeed, Network, Cpu, Gpu, Memory, Disk, Volume, Bluetooth, Battery, Power };

Mod ModFromName(const std::wstring& name);

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
namespace glyph {
constexpr wchar_t Cpu = 0xE950, Gpu = 0xE7F4, Mem = 0xE964, Disk = 0xEDA2, Temp = 0xE9CA;
constexpr wchar_t Down = 0xE896, Up = 0xE898, Eth = 0xE839, Offline = 0xE871, Bt = 0xE702;
constexpr wchar_t Mute = 0xE74F, Power = 0xE7E8, Vpn = 0xE705, Calendar = 0xE787, More = 0xE712, Window = 0xE7C4;
constexpr wchar_t Wifi[4] = { 0xE872, 0xE873, 0xE874, 0xE701 };
constexpr wchar_t Vol[4] = { 0xE992, 0xE993, 0xE994, 0xE995 };
inline wchar_t Battery(int pct, bool charging) {
    int i = std::clamp((pct + 5) / 10, 0, 10);
    if (charging) {
        if (i <= 8) return (wchar_t)(0xE85A + i);
        return i == 9 ? (wchar_t)0xE83E : (wchar_t)0xEA93;
    }
    return i < 10 ? (wchar_t)(0xE850 + i) : (wchar_t)0xE83F;
}
inline wchar_t Volume(float level) { return Vol[level <= 0.001f ? 0 : level < 0.34f ? 1 : level < 0.67f ? 2 : 3]; }
}  // namespace glyph

// The colour that identifies a module (its icon colour on the bar).
Color ModAccent(const Theme& t, Mod m);
// ISO 8601 week number (and optionally the day of the year).
int IsoWeek(const SYSTEMTIME& st, int* dayOfYear);
inline Color TempColor(const Theme& t, float c) { return c >= 85 ? t.hot : (c >= 72 ? t.warn : t.temp); }
inline Color LoadColor(const Theme& t, float v, const Color& normal) { return v < 0.6f ? normal : (v < 0.85f ? t.warn : t.hot); }
// For how full something is (memory, video memory): only warn when it is really filling up.
inline Color FillColor(const Theme& t, float v, const Color& normal) { return v < 0.8f ? normal : (v < 0.92f ? t.warn : t.hot); }

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

// --- Tooltip content ---------------------------------------------------------------------------
// A tooltip is a card: header (icon chip, title, subtitle, badge), then optional sections in this
// order: big numbers, a history chart, the per-core grid or a calendar, meters, detail rows, and
// usage hints at the bottom.

struct TipLine { std::wstring label, value; };
struct TipStat {
    std::wstring value, unit, label;
    Color color{ 0, 0, 0, 0 };  // value colour; alpha 0 = normal text
    Color dot{ 0, 0, 0, 0 };    // legend dot before the label (matches a chart series); alpha 0 = none
};
struct TipMeter { std::wstring label, value; float frac = 0; Color color; };
struct TipHint { std::wstring key, action; };
struct TipChart {
    const std::vector<float>* a = nullptr;   // main series (filled)
    const std::vector<float>* b = nullptr;   // optional second series
    Color ca, cb;
    float max = 1.f;                          // values are drawn as v / max
    std::wstring caption, summary;            // left and right above the chart
};
struct TipContent {
    wchar_t glyph = 0;
    ID2D1Bitmap* icon = nullptr;              // a window's icon instead of the glyph
    Color accent;
    std::wstring title, subtitle, badge;
    Color badgeColor;
    std::vector<TipStat> stats;
    bool hasChart = false;
    TipChart chart;
    bool coreGrid = false;
    bool calendar = false;
    std::vector<TipMeter> meters;
    std::vector<TipLine> rows;
    std::vector<TipHint> hints;
    bool Empty() const { return title.empty() && rows.empty() && stats.empty() && meters.empty(); }
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

    // Pet: the free span of the bar it may walk in (DIPs), and drawing it over the bar.
    bool PetSpan(const std::wstring& area, float petCenter, float petWidth, float& left, float& right) const;
    void PetUpdate();   // redraw just the pixels around the pet
    void PetClear();
    bool PetHitTest(int px, int py) const;

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

    // Hover highlight fades (one per module or window item that is fading in or out).
    struct Fade { Mod mod; HWND hwnd; float from, to; double start; };
    float HoverLevel(Mod m, HWND h, double now) const;
    void SetHover(int idx);
    bool FadesBusy(double now) const;

    void PetErase();             // put back the bar pixels under the pet (memory surface only)
    void PetDraw();              // blend the pet into the memory surface
    void Blit(const RECT& r);

    App& app_;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    uint32_t* bits_ = nullptr;
    int surfW_ = 0, surfH_ = 0;
    bool tracking_ = false;
    std::vector<Fade> fades_;
    bool animating_ = false;
    // Free spans between module groups, DIPs: [0] left (apps .. clock), [1] right (clock .. stats).
    float gapL_[2] = {}, gapR_[2] = {};
    RECT petRect_{};
    bool petDrawn_ = false;
    std::vector<uint32_t> petBackup_;
};

class Tooltip {
public:
    bool Create(HINSTANCE inst);
    void Show(Bar* bar, const Hit& hit);
    void Refresh();               // re-render with fresh values if visible
    void Hide(int line = 0);      // at once (menus, clicks)
    void FadeOut();               // the mouse left the bar
    bool Visible() const { return visible_; }
    Bar* Owner() const { return owner_; }
    Mod HitMod() const { return visible_ ? hit_.mod : Mod::None; }
    void Destroy();
    HWND hwnd = nullptr;
#ifdef TOPBAR_DRY_RUN
    // Test builds: render every module's card of `bar` to PNG files in `dir`.
    void Snapshot(Bar* bar, const std::wstring& dir);
#endif

private:
    bool Compose();               // lay out and render into the surface; false when there is nothing to show
    void Present(bool newContent);
    void Tick();
    void EnsureSurface(int w, int h);
    void BuildMask();
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);

    Bar* owner_ = nullptr;
    Hit hit_;
    bool visible_ = false;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    uint32_t* bits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    // Card geometry in surface pixels, and the cached coverage / shadow masks for it.
    int cardX_ = 0, cardY_ = 0, cardW_ = 0, cardH_ = 0;
    std::vector<uint8_t> cover_, shadow_;
    int maskW_ = 0, maskH_ = 0, maskCardW_ = 0, maskCardH_ = 0;
    float maskScale_ = 0;
    // Where the card sits on screen (top-left of the card, screen pixels) and the motion towards it.
    enum class Anim { None, Open, Glide, Close };
    Anim anim_ = Anim::None;
    double animStart_ = 0;
    POINT target_{}, from_{};
    POINT shown_{};               // last presented card position
    float alpha_ = 0, fromAlpha_ = 0;
    // Keeps the card from shrinking and jittering while the same module stays hovered.
    Mod widthMod_ = Mod::None;
    HWND widthHwnd_ = nullptr;
    float minW_ = 0;
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
    Pet pet;
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
    bool Animations() const { return cfg.animations && animationsOn_; }
    Bar* PetBar() const;          // the bar the pet lives on (primary monitor)

    // Module content & actions (used by bars/tooltips)
    void BuildTooltip(const Hit& hit, TipContent& c);
    void TurnDisplayOffSoon();
    void TrimMemorySoon();
    void UpdateCpuCounterDemand();
    void OnModuleClick(Bar& bar, const Hit& hit, int button, POINT pt);
    void OnModuleWheel(const Hit& hit, int delta);
    void ShowContextMenu(POINT pt);
    void PetHop();

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
    void SyncAutostart();
    static bool AutostartDisabledByUser();
    static void ClearAutostartDisabled();
    bool AnyBarVisible() const;
    UINT CurrentInterval() const;

    // Pet
    void ApplyPet(bool interactive = false);
    void PetTick();
    void UpdatePetTimer();
    void ChoosePetFile();
    std::wstring petKey_;         // what the current sprite was made from
    unsigned petGen_ = 0;
    int petPx_ = 0;               // sprite height in pixels
    bool petInteractive_ = false; // the file was just picked: explain if it can't be used

    // Colours from the wallpaper
    void CheckWallpaper(bool force);
    void ApplyThemeOverride();
    void StartThemeFade(const Theme& to);
    std::wstring wallSig_;
    unsigned wallGen_ = 0;
    Theme iniTheme_{};            // the colours from topbar.ini
    Theme wallTheme_{};
    bool wallThemeValid_ = false;
    Theme fadeFrom_{}, fadeTo_{};
    double fadeStart_ = -1;

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
    bool animationsOn_ = true;     // Windows' animation setting
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
