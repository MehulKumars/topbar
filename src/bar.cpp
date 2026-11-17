// A bar on one monitor: appbar registration, layout, drawing and input. Plus the tooltip popup.
#include "app.h"

namespace {

enum : UINT_PTR { T_TIP = 10 };

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
constexpr wchar_t G_CPU = 0xE950, G_GPU = 0xE7F4, G_MEM = 0xE964, G_DISK = 0xEDA2, G_TEMP = 0xE9CA;
constexpr wchar_t G_DOWN = 0xE896, G_UP = 0xE898, G_ETH = 0xE839, G_OFFLINE = 0xE871, G_BT = 0xE702;
constexpr wchar_t G_MUTE = 0xE74F, G_POWER = 0xE7E8, G_VPN = 0xE705;
constexpr wchar_t G_WIFI[4] = { 0xE872, 0xE873, 0xE874, 0xE701 };
constexpr wchar_t G_VOL[4] = { 0xE992, 0xE993, 0xE994, 0xE995 };

wchar_t BatteryGlyph(int pct, bool charging) {
    int i = std::clamp((pct + 5) / 10, 0, 10);
    if (charging) {
        if (i <= 8) return (wchar_t)(0xE85A + i);
        return i == 9 ? (wchar_t)0xE83E : (wchar_t)0xEA93;
    }
    return i < 10 ? (wchar_t)(0xE850 + i) : (wchar_t)0xE83F;
}

struct Span {
    enum Kind { Icon, Text, Bold, Sub, Bars } kind = Text;
    std::wstring text;
    Color color;
    float minW = 0;
    std::vector<float> bars;
    TextRun run;     // shaped once per frame
};
using Spans = std::vector<Span>;

constexpr float kPad = 10.f;     // inner horizontal padding of a module
constexpr float kGap = 6.f;      // gap between modules
constexpr float kMargin = 6.f;   // bar edge margin
constexpr float kVPad = 3.f;     // vertical inset of pills
constexpr float kBarW = 2.f, kBarGap = 1.f, kBarH = 13.f;

Font SpanFont(Span::Kind k) {
    switch (k) {
    case Span::Icon: return Font::Icon;
    case Span::Bold: return Font::Bold;
    case Span::Sub: return Font::Sub;
    default: return Font::Text;
    }
}

float SpanWidth(const Span& s) {
    if (s.kind == Span::Bars) return s.bars.empty() ? 0 : s.bars.size() * (kBarW + kBarGap) - kBarGap;
    return std::max(s.minW, s.run.width);
}

// Shape every text span once; widths and drawing reuse the layouts.
void PrepareSpans(Renderer& r, Spans& spans, float height) {
    for (auto& s : spans)
        if (s.kind != Span::Bars) s.run = r.MakeRun(s.text, SpanFont(s.kind), height);
}

float GapBefore(const Span* prev, const Span& s) {
    if (!prev) return 0;
    if (prev->kind == Span::Icon) return 5.f;
    if (s.kind == Span::Icon) return 9.f;
    if (s.kind == Span::Bars || prev->kind == Span::Bars) return 6.f;
    return 4.f;
}

float SpansWidth(const Spans& spans) {
    float w = 0;
    for (size_t i = 0; i < spans.size(); ++i) w += GapBefore(i ? &spans[i - 1] : nullptr, spans[i]) + SpanWidth(spans[i]);
    return w;
}

void DrawSpans(Renderer& r, const Theme& t, const Spans& spans, float x, float top, float bottom) {
    for (size_t i = 0; i < spans.size(); ++i) {
        const Span& s = spans[i];
        x += GapBefore(i ? &spans[i - 1] : nullptr, s);
        float w = SpanWidth(s);
        if (s.kind == Span::Bars) {
            float cy = (top + bottom) / 2, h = kBarH;
            float bx = x;
            for (float v : s.bars) {
                D2D1_RECT_F bg{ bx, cy - h / 2, bx + kBarW, cy + h / 2 };
                r.FillRect(bg, t.pillHover);
                float fh = std::max(1.f, h * Clamp01(v));
                Color c = v < 0.6f ? s.color : (v < 0.85f ? t.warn : t.hot);
                r.FillRect({ bx, bg.bottom - fh, bx + kBarW, bg.bottom }, c);
                bx += kBarW + kBarGap;
            }
        } else {
            // Numbers are right-aligned inside their fixed width so the bar doesn't jitter.
            float off = (s.minW > 0 && s.kind != Span::Icon) ? w - s.run.width : 0.f;
            // Icons sit 0.5 DIP lower to line up optically with text.
            float dy = s.kind == Span::Icon ? 0.5f : 0.f;
            r.DrawRun(s.run, x + off, top + dy, s.color);
        }
        x += w;
    }
}

Color TempColor(const Theme& t, float c) { return c >= 85 ? t.hot : (c >= 72 ? t.warn : t.temp); }

std::wstring ClockTime(const Config& cfg, const SYSTEMTIME& st) {
    wchar_t buf[64];
    const wchar_t* fmt = nullptr;
    if (cfg.clock24h == L"1") fmt = cfg.clockSeconds ? L"HH':'mm':'ss" : L"HH':'mm";
    else if (cfg.clock24h == L"0") fmt = cfg.clockSeconds ? L"h':'mm':'ss tt" : L"h':'mm tt";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, (fmt || cfg.clockSeconds) ? 0 : TIME_NOSECONDS, &st, fmt, buf, _countof(buf));
    return buf;
}

Spans BuildSpans(App& a, Mod mod) {
    const Theme& t = a.cfg.theme;
    const Stats& s = a.stats;
    Spans out;
    auto icon = [&](wchar_t g, const Color& c) { Span sp; sp.kind = Span::Icon; sp.text = std::wstring(1, g); sp.color = c; out.push_back(sp); };
    auto text = [&](const std::wstring& v, const Color& c, float minW = 0) { Span sp; sp.kind = Span::Text; sp.text = v; sp.color = c; sp.minW = minW; out.push_back(sp); };
    auto sub = [&](const std::wstring& v) { Span sp; sp.kind = Span::Sub; sp.text = v; sp.color = t.subtext; out.push_back(sp); };
    auto pct = [](float v) { return Fmt(L"%.0f%%", v * 100.f); };

    switch (mod) {
    case Mod::Clock: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t date[64] = L"";
        if (!a.cfg.dateFormat.empty())
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, a.cfg.dateFormat.c_str(), date, _countof(date), nullptr);
        if (date[0]) { Span d; d.kind = Span::Sub; d.text = date; d.color = t.subtext; out.push_back(d); }
        Span tm; tm.kind = Span::Bold; tm.text = ClockTime(a.cfg, st); tm.color = t.clock; out.push_back(tm);
        break;
    }
    case Mod::NetSpeed:
        icon(G_DOWN, t.network);
        text(FormatRate(s.rxRate, a.cfg.netBits), t.text, a.wRate);
        icon(G_UP, t.network);
        text(FormatRate(s.txRate, a.cfg.netBits), t.text, a.wRate);
        break;
    case Mod::Network: {
        const NetInfo& n = s.net;
        if (n.kind == NetInfo::None) {
            icon(G_OFFLINE, t.hot);
            text(L"Offline", t.hot);
            break;
        }
        if (n.kind == NetInfo::Wifi) {
            int lvl = n.signal < 0 ? 3 : std::clamp(n.signal / 25, 0, 3);
            icon(G_WIFI[lvl], t.network);
            text(n.ssid.empty() ? L"Wi-Fi" : (n.ssid.size() > 18 ? n.ssid.substr(0, 17) + L"…" : n.ssid), t.text);
        } else if (n.kind == NetInfo::Ethernet) {
            icon(G_ETH, t.network);
            text(L"Ethernet", t.text);
        } else {
            icon(G_VPN, t.network);
            text(n.adapterName.size() > 14 ? n.adapterName.substr(0, 13) + L"…" : n.adapterName, t.text);
        }
        if (!n.internet) sub(L"no internet");
        break;
    }
    case Mod::Cpu:
        icon(G_CPU, t.cpu);
        text(pct(s.cpu), s.cpu > 0.9f ? t.hot : t.text, a.wPct);
        if (a.cfg.cpuCoreBars && s.cores.size() > 1 && s.cores.size() <= 64) {
            Span b; b.kind = Span::Bars; b.bars = s.cores; b.color = t.cpu; out.push_back(b);
        }
        if (!std::isnan(s.cpuTemp)) {
            icon(G_TEMP, TempColor(t, s.cpuTemp));
            text(Fmt(L"%.0f°", s.cpuTemp), s.cpuTemp >= 85 ? t.hot : t.text, a.wTemp);
        }
        break;
    case Mod::Gpu: {
        if (s.gpus.empty()) break;
        std::vector<int> which;
        if (a.cfg.gpuSelect == L"all") for (int i = 0; i < (int)s.gpus.size(); ++i) which.push_back(i);
        else if (!a.cfg.gpuSelect.empty() && iswdigit(a.cfg.gpuSelect[0])) {
            int i = _wtoi(a.cfg.gpuSelect.c_str());
            which.push_back(i >= 0 && i < (int)s.gpus.size() ? i : std::max(s.mainGpu, 0));
        } else which.push_back(std::max(s.mainGpu, 0));
        for (int i : which) {
            const GpuStat& g = s.gpus[i];
            icon(G_GPU, t.gpu);
            text(pct(g.usage), g.usage > 0.95f ? t.hot : t.text, a.wPct);
            if (!std::isnan(g.temp)) {
                icon(G_TEMP, TempColor(t, g.temp));
                text(Fmt(L"%.0f°", g.temp), g.temp >= 85 ? t.hot : t.text, a.wTemp);
            }
        }
        break;
    }
    case Mod::Memory:
        icon(G_MEM, t.memory);
        text(pct(s.memLoad), s.memLoad > 0.9f ? t.hot : t.text, a.wPct);
        sub(Fmt(L"%.1fG", s.memUsed / 1073741824.0));
        break;
    case Mod::Disk: {
        for (auto& d : s.drives) {
            if (d.letter != s.systemDrive || !d.total) continue;
            float used = 1.f - (float)((double)d.free / (double)d.total);
            icon(G_DISK, t.disk);
            text(pct(used), used > 0.92f ? t.hot : t.text, a.wPct);
            sub(Fmt(L"%c:", d.letter));
        }
        break;
    }
    case Mod::Volume: {
        if (!a.audio.present) break;
        if (a.audio.muted) { icon(G_MUTE, t.subtext); text(L"Muted", t.subtext); break; }
        float l = a.audio.level;
        icon(G_VOL[l <= 0.001f ? 0 : l < 0.34f ? 1 : l < 0.67f ? 2 : 3], t.volume);
        text(pct(l), t.text, a.wPct);
        break;
    }
    case Mod::Bluetooth: {
        const BtInfo& b = s.bt;
        if (!b.present) break;
        if (!b.on) { icon(G_BT, t.subtext); break; }
        icon(G_BT, t.bluetooth);
        if (!b.connected.empty()) {
            std::wstring n = b.connected[0];
            if (n.size() > 16) n = n.substr(0, 15) + L"…";
            text(n, t.text);
            if (b.connected.size() > 1) sub(Fmt(L"+%zu", b.connected.size() - 1));
        }
        break;
    }
    case Mod::Battery: {
        const BatteryInfo& b = s.battery;
        if (!b.present || b.percent < 0) break;
        Color c = b.charging || b.onAC ? t.battery : (b.percent <= 15 ? t.hot : (b.percent <= 30 ? t.warn : t.battery));
        icon(BatteryGlyph(b.percent, b.charging), c);
        text(Fmt(L"%d%%", b.percent), b.percent <= 15 && !b.onAC ? t.hot : t.text, a.wPct);
        break;
    }
    case Mod::Power:
        icon(G_POWER, t.power);
        break;
    default:
        break;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Bar::Bar(App& app, const MonitorDesc& m) : mon(m), app_(app) {}

Bar::~Bar() {
    if (hwnd) {
        KillTimer(hwnd, T_TIP);
        UnregisterAppBar();
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    FreeSurface();
}

bool Bar::Create() {
    pxHeight = (int)std::lround(app_.cfg.height * Scale());
    pxWidth = mon.rect.right - mon.rect.left;
    barRect = { mon.rect.left, mon.rect.top, mon.rect.right, mon.rect.top + pxHeight };
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"TopBarWnd", TOPBAR_NAME, WS_POPUP,
                           barRect.left, barRect.top, pxWidth, pxHeight, nullptr, nullptr, app_.inst, this);
    if (!hwnd) return false;
    BOOL yes = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));
    DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &yes, sizeof(yes));
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    return true;
}

void Bar::SetMonitor(const MonitorDesc& m) {
    mon = m;
    Reposition();
}

void Bar::RegisterAppBar() {
    if (appbarRegistered || !hwnd) return;
    APPBARDATA abd{ sizeof(abd) };
    abd.hWnd = hwnd;
    abd.uCallbackMessage = WM_APP_APPBAR;
    appbarRegistered = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
}

void Bar::UnregisterAppBar() {
    if (!appbarRegistered || !hwnd) return;
    APPBARDATA abd{ sizeof(abd) };
    abd.hWnd = hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
    appbarRegistered = false;
}

void Bar::Reposition() {
    if (!hwnd) return;
    pxHeight = (int)std::lround(app_.cfg.height * Scale());
    RECT r = mon.rect;
    r.bottom = r.top + pxHeight;
    if (appbarRegistered) {
        APPBARDATA abd{ sizeof(abd) };
        abd.hWnd = hwnd;
        abd.uEdge = ABE_TOP;
        abd.rc = r;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + pxHeight;
        SHAppBarMessage(ABM_SETPOS, &abd);
        r = abd.rc;
    }
    barRect = r;
    pxWidth = r.right - r.left;

    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (app_.cfg.opacity < 100) {
        if (!(ex & WS_EX_LAYERED)) SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)(app_.cfg.opacity * 255 / 100), LWA_ALPHA);
    } else if (ex & WS_EX_LAYERED) {
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
    SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, pxWidth, pxHeight, SWP_NOACTIVATE);
    if (visible) Render();
}

void Bar::SetVisible(bool show) {
    if (show == visible || !hwnd) return;
    visible = show;
    if (show) {
        Render();
        SetWindowPos(hwnd, HWND_TOPMOST, barRect.left, barRect.top, pxWidth, pxHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        HideTip();
        ShowWindow(hwnd, SW_HIDE);
    }
}

void Bar::HideTip() {
    if (app_.tip.Owner() == this) app_.tip.Hide();
}

void Bar::EnsureSurface() {
    if (memDC_ && surfW_ == pxWidth && surfH_ == pxHeight) return;
    FreeSurface();
    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), std::max(pxWidth, 1), -std::max(pxHeight, 1), 1, 32, BI_RGB };
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    oldBmp_ = SelectObject(memDC_, dib_);
    surfW_ = pxWidth;
    surfH_ = pxHeight;
}

void Bar::FreeSurface() {
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    surfW_ = surfH_ = 0;
}

int Bar::HitTest(int px, int py) const {
    float x = px / Scale(), y = py / Scale();
    for (size_t i = 0; i < hits.size(); ++i) {
        const auto& r = hits[i].rect;
        if (x >= r.left && x < r.right && y >= 0 && y < pxHeight / Scale()) return (int)i;
    }
    return -1;
}

void Bar::Render() {
    if (!hwnd || pxWidth <= 0 || pxHeight <= 0) return;
    EnsureSurface();
    Renderer& r = app_.renderer;
    const Config& cfg = app_.cfg;
    const Theme& t = cfg.theme;
    float s = Scale();
    float W = pxWidth / s, H = pxHeight / s;
    float top = kVPad, bottom = H - kVPad;
    float radius = std::min(8.f, (bottom - top) / 2);

    // Remember what is hovered so the highlight survives re-layout.
    Mod hoverMod = Mod::None;
    HWND hoverHwnd = nullptr;
    if (hover >= 0 && hover < (int)hits.size()) { hoverMod = hits[hover].mod; hoverHwnd = hits[hover].hwnd; }
    std::vector<Hit> newHits;
    auto isHover = [&](Mod m, HWND h) { return m == hoverMod && h == hoverHwnd; };

    if (!r.Begin(memDC_, pxWidth, pxHeight, (float)mon.dpi)) return;
    r.Clear(t.background);
    r.FillRect({ 0, H - 1.f / s, W, H }, t.pill);

    struct Item { Mod mod; Spans spans; float w = 0; };
    auto build = [&](const std::vector<std::wstring>& names) {
        std::vector<Item> v;
        for (auto& n : names) {
            Mod m = ModFromName(n);
            if (m == Mod::None) continue;
            Item it{ m };
            if (m != Mod::Windows) {
                it.spans = BuildSpans(app_, m);
                if (it.spans.empty()) continue;
                PrepareSpans(r, it.spans, bottom - top);
                it.w = SpansWidth(it.spans) + 2 * kPad;
            }
            v.push_back(std::move(it));
        }
        return v;
    };
    auto drawModule = [&](const Item& it, float x) {
        D2D1_RECT_F rc{ x, top, x + it.w, bottom };
        r.FillRound(rc, radius, isHover(it.mod, nullptr) ? t.pillHover : t.pill);
        DrawSpans(r, t, it.spans, x + kPad, top, bottom);
        newHits.push_back({ rc, it.mod, nullptr });
    };

    // Right group, laid out from the right edge.
    std::vector<Item> right = build(cfg.modulesRight);
    float x = W - kMargin;
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
        if (it->mod == Mod::Windows) continue;
        x -= it->w;
        drawModule(*it, x);
        x -= kGap;
    }
    float rightEdge = x;

    // Centre group, centred on the monitor but never overlapping the right group.
    std::vector<Item> center = build(cfg.modulesCenter);
    float cw = 0;
    for (auto& it : center) if (it.mod != Mod::Windows) cw += it.w + kGap;
    if (cw > 0) cw -= kGap;
    float cx = std::min(W / 2 - cw / 2, rightEdge - cw);
    float centerLeft = cw > 0 ? cx : rightEdge;
    for (auto& it : center) {
        if (it.mod == Mod::Windows) continue;
        drawModule(it, cx);
        cx += it.w + kGap;
    }

    // Left group; the window list takes whatever room is left.
    std::vector<Item> left = build(cfg.modulesLeft);
    float fixed = 0;
    for (auto& it : left) if (it.mod != Mod::Windows) fixed += it.w + kGap;
    float lx = kMargin;
    float leftLimit = centerLeft - kGap * 2;
    for (auto& it : left) {
        if (it.mod != Mod::Windows) {
            drawModule(it, lx);
            lx += it.w + kGap;
            continue;
        }
        float avail = leftLimit - lx - (fixed - (lx - kMargin));
        // Collect this monitor's windows.
        std::vector<WinItem*> wins;
        bool perMon = cfg.windowsPerMonitor && app_.bars.size() > 1;
        for (auto& w : app_.windows.items)
            if (!perMon || w.monitor == mon.handle) wins.push_back(&w);
        if (wins.empty()) continue;

        const float iconSz = 18.f, ipad = 7.f, igap = 2.f;
        HWND active = app_.windows.Active();
        float itemW = ipad * 2 + iconSz;
        WinItem* act = nullptr;
        for (auto* w : wins) if (w->hwnd == active) act = w;
        float titleW = 0;
        if (act && cfg.showActiveTitle && cfg.maxTitleWidth > 0)
            titleW = std::min((float)cfg.maxTitleWidth, r.Measure(act->title, Font::Text));
        auto total = [&](size_t n, float tw) { return n * itemW + (n ? (n - 1) * igap : 0) + (tw > 0 ? tw + 6.f : 0); };
        if (total(wins.size(), titleW) > avail) titleW = std::max(0.f, avail - total(wins.size(), 0) - 6.f);
        if (titleW < 40.f) titleW = 0;
        size_t shown = wins.size();
        float overflowW = itemW;
        if (total(shown, titleW) > avail) {
            while (shown > 0 && total(shown, titleW) + igap + overflowW > avail) --shown;
            // Keep the active window visible.
            if (act) {
                auto pos = std::find(wins.begin(), wins.end(), act) - wins.begin();
                if ((size_t)pos >= shown && shown > 0) std::swap(wins[shown - 1], wins[pos]);
            }
        }
        for (size_t i = 0; i < shown; ++i) {
            WinItem* w = wins[i];
            bool isAct = w->hwnd == active;
            float wdt = itemW + (isAct && titleW > 0 ? titleW + 6.f : 0);
            D2D1_RECT_F rc{ lx, top, lx + wdt, bottom };
            if (isAct) r.FillRound(rc, radius, isHover(Mod::Windows, w->hwnd) ? t.pillHover : t.pillActive);
            else if (isHover(Mod::Windows, w->hwnd)) r.FillRound(rc, radius, t.pillHover);
            else if (w->flashing) r.FillRound(rc, radius, t.warn.WithAlpha(0.35f));
            float cy = (top + bottom) / 2;
            D2D1_RECT_F ir{ lx + ipad, cy - iconSz / 2, lx + ipad + iconSz, cy + iconSz / 2 };
            if (w->icon) {
                r.Bitmap(w->icon.Get(), ir, w->minimized ? 0.45f : 1.f);
            } else {
                r.FillRound(ir, 4.f, t.accent.WithAlpha(w->minimized ? 0.4f : 0.85f));
                std::wstring letter = w->exe.empty() ? L"?" : std::wstring(1, (wchar_t)towupper(w->exe[0]));
                r.Text(letter, Font::Bold, ir, t.background, Align::Center);
            }
            if (isAct) {
                float mid = lx + ipad + iconSz / 2;
                r.FillRound({ mid - 6.f, bottom - 2.5f, mid + 6.f, bottom - 0.5f }, 1.f, t.accent);
                if (titleW > 0)
                    r.Text(w->title, Font::Text, { ir.right + 6.f, top, ir.right + 6.f + titleW, bottom }, t.text, Align::Left, true);
            } else if (!w->minimized) {
                float mid = lx + ipad + iconSz / 2;
                r.FillRound({ mid - 1.5f, bottom - 2.5f, mid + 1.5f, bottom - 0.5f }, 1.f, t.subtext);
            }
            newHits.push_back({ rc, Mod::Windows, w->hwnd });
            lx += wdt + igap;
        }
        if (shown < wins.size()) {
            D2D1_RECT_F rc{ lx, top, lx + overflowW, bottom };
            r.FillRound(rc, radius, isHover(Mod::WindowOverflow, nullptr) ? t.pillHover : t.pill);
            r.Text(Fmt(L"+%zu", wins.size() - shown), Font::Text, rc, t.subtext, Align::Center);
            newHits.push_back({ rc, Mod::WindowOverflow, nullptr });
            lx += overflowW + igap;
        }
        lx += kGap - igap;
    }

    r.End();
    hits = std::move(newHits);
    hover = -1;
    for (size_t i = 0; i < hits.size(); ++i)
        if (hits[i].mod == hoverMod && hits[i].hwnd == hoverHwnd && hoverMod != Mod::None) hover = (int)i;

    if (visible) {
        HDC dc = GetDC(hwnd);
        BitBlt(dc, 0, 0, pxWidth, pxHeight, memDC_, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }
}

void Bar::OnClick(int idx, int button, POINT pt) {
    HideTip();
    if (idx < 0 || idx >= (int)hits.size()) {
        if (button == 1) app_.ShowContextMenu(pt);
        return;
    }
    Hit hit = hits[idx];
    app_.OnModuleClick(*this, hit, button, pt);
}

LRESULT CALLBACK Bar::StaticProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Bar* self = nullptr;
    if (m == WM_NCCREATE) {
        self = (Bar*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = h;
    } else {
        self = (Bar*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    if (self) return self->Proc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT Bar::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (memDC_) BitBlt(dc, 0, 0, pxWidth, pxHeight, memDC_, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        int idx = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx != hover) {
            hover = idx;
            Render();
            if (app_.tip.Visible()) {
                if (idx >= 0) app_.tip.Show(this, hits[idx]);
                else app_.tip.Hide();
            } else {
                KillTimer(hwnd, T_TIP);
                if (idx >= 0) SetTimer(hwnd, T_TIP, 450, nullptr);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        KillTimer(hwnd, T_TIP);
        HideTip();
        if (hover != -1) { hover = -1; Render(); }
        return 0;
    case WM_TIMER:
        if (w == T_TIP) {
            KillTimer(hwnd, T_TIP);
            if (hover >= 0 && hover < (int)hits.size()) app_.tip.Show(this, hits[hover]);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int idx = HitTest(pt.x, pt.y);
        ClientToScreen(hwnd, &pt);
        // Menus open just below the bar.
        pt.y = barRect.bottom;
        OnClick(idx, m == WM_LBUTTONUP ? 0 : (m == WM_RBUTTONUP ? 1 : 2), pt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(hwnd, &pt);
        int idx = HitTest(pt.x, pt.y);
        if (idx >= 0) app_.OnModuleWheel(hits[idx], GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    case WM_APP_APPBAR:
        if (w == ABN_POSCHANGED) Reposition();
        else if (w == ABN_FULLSCREENAPP) app_.RequestWatch();
        return 0;
    case WM_ACTIVATE: {
        APPBARDATA abd{ sizeof(abd) };
        abd.hWnd = hwnd;
        if (appbarRegistered) SHAppBarMessage(ABM_ACTIVATE, &abd);
        break;
    }
    case WM_WINDOWPOSCHANGED: {
        APPBARDATA abd{ sizeof(abd) };
        abd.hWnd = hwnd;
        if (appbarRegistered) SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
        break;
    }
    case WM_DPICHANGED: {
        UINT dpi = HIWORD(w);
        if (dpi != mon.dpi) { mon.dpi = dpi; Reposition(); }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// --- Tooltip ---------------------------------------------------------------------------------

bool Tooltip::Create(HINSTANCE inst) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = Proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TopBarTip";
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"TopBarTip", L"",
                           WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, this);
    if (!hwnd) return false;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    BOOL yes = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));
    return true;
}

void Tooltip::Show(Bar* bar, const Hit& hit) {
    if (!hwnd || !bar) return;
    owner_ = bar;
    hit_ = hit;
    visible_ = true;
    Draw();
}

void Tooltip::Refresh() {
    if (visible_ && owner_) Draw();
}

void Tooltip::Hide() {
    if (!hwnd) return;
    if (visible_) ShowWindow(hwnd, SW_HIDE);
    visible_ = false;
    owner_ = nullptr;
}

void Tooltip::Draw() {
    App& a = *App::I;
    std::wstring title;
    std::vector<TipLine> lines;
    a.BuildTooltip(hit_, title, lines);
    if (title.empty() && lines.empty()) { Hide(); return; }
    Renderer& r = a.renderer;
    const Theme& t = a.cfg.theme;
    Bar& bar = *owner_;
    float s = bar.Scale();

    const float pad = 12.f, colGap = 18.f, maxW = 560.f;
    if (title.size() > 90) title = title.substr(0, 87) + L"...";
    float titleH = r.LineHeight(Font::TipBold);
    float lineH = r.LineHeight(Font::Tip) + 3.f;
    float labelW = 0, valueW = 0, hintW = 0;
    for (auto& ln : lines) {
        if (ln.label.empty() && !ln.value.empty() && &ln == &lines.back()) hintW = std::max(hintW, r.Measure(ln.value, Font::Sub));
        else { labelW = std::max(labelW, r.Measure(ln.label, Font::Tip)); valueW = std::max(valueW, r.Measure(ln.value, Font::Tip)); }
    }
    float contentW = std::max({ r.Measure(title, Font::TipBold), labelW + colGap + valueW, hintW });
    contentW = std::min(contentW, maxW);
    float w = contentW + pad * 2;
    float h = pad + titleH + (lines.empty() ? 0 : 8.f + lines.size() * lineH) + pad - 2.f;

    int pw = (int)std::ceil(w * s), ph = (int)std::ceil(h * s);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), pw, -ph, 1, 32, BI_RGB };
    if (!memDC_) {
        HDC screen = GetDC(nullptr);
        memDC_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    if (oldBmp_) SelectObject(memDC_, oldBmp_);
    if (dib_) DeleteObject(dib_);
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    oldBmp_ = SelectObject(memDC_, dib_);

    if (!r.Begin(memDC_, pw, ph, (float)bar.mon.dpi)) return;
    r.Clear(t.pill);
    r.StrokeRound({ 0.5f / s, 0.5f / s, w - 0.5f / s, h - 0.5f / s }, 7.f, t.pillActive, 1.f / s);
    float y = pad - 2.f;
    r.Text(title, Font::TipBold, { pad, y, pad + contentW, y + titleH }, t.text, Align::Left, true);
    y += titleH + 4.f;
    if (!lines.empty()) {
        r.FillRect({ pad, y, w - pad, y + 1.f / s }, t.pillActive);
        y += 4.f;
        for (size_t i = 0; i < lines.size(); ++i) {
            const TipLine& ln = lines[i];
            bool hint = ln.label.empty() && i + 1 == lines.size();
            if (hint) {
                r.Text(ln.value, Font::Sub, { pad, y, pad + contentW, y + lineH }, t.subtext, Align::Left, true);
            } else {
                r.Text(ln.label, Font::Tip, { pad, y, pad + labelW, y + lineH }, t.subtext, Align::Left, true);
                r.Text(ln.value, Font::Tip, { pad + labelW + colGap, y, pad + contentW, y + lineH }, t.text, Align::Left, true);
            }
            y += lineH;
        }
    }
    r.End();

    // Position under the hovered module, kept on the bar's monitor.
    int cx = bar.barRect.left + (int)(((hit_.rect.left + hit_.rect.right) / 2) * s);
    int x = cx - pw / 2;
    int yPos = bar.barRect.bottom + (int)(6 * s);
    x = std::clamp(x, (int)bar.mon.rect.left + (int)(6 * s), (int)bar.mon.rect.right - pw - (int)(6 * s));
    SetWindowPos(hwnd, HWND_TOPMOST, x, yPos, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HDC dc = GetDC(hwnd);
    BitBlt(dc, 0, 0, pw, ph, memDC_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK Tooltip::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = (Tooltip*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (self && self->memDC_) {
            RECT rc;
            GetClientRect(h, &rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, self->memDC_, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
