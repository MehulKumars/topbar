// Tooltip cards. Each hover popup is a layered window: the card is drawn with ClearType text into an
// opaque surface, then given true anti-aliased rounded corners and a soft two-layer shadow through a
// per-pixel alpha mask (computed once per card size). It fades and slides in, glides between modules
// and fades out, by moving the finished picture only, so motion costs no drawing at all.
#include "app.h"

namespace {

constexpr float kPad = 14.f;         // card padding
constexpr float kRadius = 10.f;      // card corners
constexpr float kChip = 30.f;        // header icon chip
constexpr float kSection = 14.f;     // space between sections
constexpr float kMinW = 236.f, kMaxW = 440.f;
constexpr float kMarginX = 18.f, kMarginTop = 12.f, kMarginBottom = 26.f;  // room for the shadow
constexpr float kOpenSeconds = 0.18f, kGlideSeconds = 0.16f, kCloseSeconds = 0.11f;
constexpr UINT_PTR T_ANIM = 1;

struct Ctx {
    Renderer& r;
    const Theme& t;
    const Stats& st;
    const TipContent& c;
    float x0, cw;     // content left edge and width (DIPs, card coordinates)
    float px;         // one physical pixel in DIPs
    bool draw;        // false: only measure
};

float LH(Renderer& r, Font f) { return r.LineHeight(f); }

// Text whose baseline sits at `baseline`.
void TextBase(Renderer& r, const std::wstring& s, Font f, float x, float baseline, float w, const Color& c,
              Align a = Align::Left) {
    float top = baseline - r.Baseline(f);
    r.Text(s, f, { x, top, x + w, top + LH(r, f) }, c, a, true);
}

Color Tint(const Theme& t, const Color& c, float k) { return Color::Lerp(t.pill, c, k); }

// --- Header: icon chip, title + subtitle, badge -------------------------------------------------

float Header(Ctx& k, float y) {
    Renderer& r = k.r;
    const TipContent& c = k.c;
    float titleH = LH(r, Font::TipBold), subH = c.subtitle.empty() ? 0.f : LH(r, Font::TipSmall);
    float textH = titleH + (subH > 0 ? subH : 0.f);
    float h = std::max(kChip, textH);
    if (!k.draw) return h;
    const Theme& t = k.t;
    float cy = y + h / 2;
    D2D1_RECT_F chip{ k.x0, cy - kChip / 2, k.x0 + kChip, cy + kChip / 2 };
    if (c.icon) {
        r.FillRound(chip, 8.f, Tint(t, t.pillHover, 1.f));
        r.Bitmap(c.icon, { chip.left + 6.f, chip.top + 6.f, chip.right - 6.f, chip.bottom - 6.f });
    } else {
        r.FillRound(chip, 8.f, Tint(t, c.accent, 0.17f));
        r.Text(std::wstring(1, c.glyph), Font::IconBig, { chip.left, chip.top + 0.5f, chip.right, chip.bottom + 0.5f },
               c.accent, Align::Center);
    }
    float tx = chip.right + 11.f, right = k.x0 + k.cw;
    float ty = y + (h - textH) / 2;
    float badgeW = 0;
    if (!c.badge.empty()) {
        float bw = r.Measure(c.badge, Font::TipKey) + 16.f, bh = 18.f;
        float by = ty + (titleH - bh) / 2;
        D2D1_RECT_F b{ right - bw, by, right, by + bh };
        r.FillRound(b, bh / 2, Tint(t, c.badgeColor, 0.18f));
        r.Text(c.badge, Font::TipKey, b, c.badgeColor, Align::Center);
        badgeW = bw + 10.f;
    }
    r.Text(c.title, Font::TipBold, { tx, ty, right - badgeW, ty + titleH }, t.text, Align::Left, true);
    if (subH > 0)
        r.Text(c.subtitle, Font::TipSmall, { tx, ty + titleH, right, ty + titleH + subH }, t.subtext, Align::Left, true);
    return h;
}

// --- Big numbers ------------------------------------------------------------------------------

constexpr float kStatGap = 30.f;

float StatWidth(Renderer& r, const TipStat& s) {
    float v = r.Measure(s.value, Font::TipHero) + (s.unit.empty() ? 0.f : 3.f + r.Measure(s.unit, Font::TipUnit));
    float l = r.Measure(s.label, Font::TipSmall) + (s.dot.a > 0 ? 11.f : 0.f);
    return std::ceil(std::max(v, l));
}

float StatsBlock(Ctx& k, float y) {
    const auto& stats = k.c.stats;
    if (stats.empty()) return 0;
    Renderer& r = k.r;
    float base = r.Baseline(Font::TipHero), smallH = LH(r, Font::TipSmall);
    float h = base + 4.f + smallH;
    if (!k.draw) return h;
    const Theme& t = k.t;
    float x = k.x0, baseY = y + base;
    for (size_t i = 0; i < stats.size(); ++i) {
        const TipStat& s = stats[i];
        float w = StatWidth(r, s);
        float vw = r.Measure(s.value, Font::TipHero);
        TextBase(r, s.value, Font::TipHero, x, baseY, vw + 4.f, s.color.a > 0 ? s.color : t.text);
        if (!s.unit.empty()) TextBase(r, s.unit, Font::TipUnit, x + vw + 3.f, baseY, 120.f, t.subtext);
        float ly = baseY + 4.f, lx = x;
        if (s.dot.a > 0) {
            r.FillEllipse({ lx + 3.5f, ly + smallH / 2 }, 3.5f, 3.5f, s.dot);
            lx += 11.f;
        }
        r.Text(s.label, Font::TipSmall, { lx, ly, x + w + 2.f, ly + smallH }, t.subtext);
        x += w;
        if (i + 1 < stats.size()) {
            float dx = std::round(x + kStatGap / 2);
            r.FillRect({ dx, y + 6.f, dx + k.px, y + h - 2.f }, Tint(t, t.pillActive, 0.7f));
            x += kStatGap;
        }
    }
    return h;
}

// --- History chart ----------------------------------------------------------------------------

float ChartBlock(Ctx& k, float y) {
    const TipChart& ch = k.c.chart;
    if (!k.c.hasChart || !ch.a || ch.a->size() < 2) return 0;
    Renderer& r = k.r;
    float capH = LH(r, Font::TipSmall), chartH = 52.f;
    float h = capH + 6.f + chartH;
    if (!k.draw) return h;
    const Theme& t = k.t;
    float x0 = k.x0, x1 = k.x0 + k.cw;
    r.Text(ch.caption, Font::TipSmallBold, { x0, y, x1, y + capH }, t.subtext);
    r.Text(ch.summary, Font::TipSmall, { x0, y, x1, y + capH }, t.subtext, Align::Right);
    float top = y + capH + 6.f, bottom = top + chartH;
    r.FillRound({ x0, top, x1, bottom }, 6.f, Color::Lerp(t.pill, t.background, 0.5f));
    for (int i = 1; i <= 3; ++i) {
        float gy = std::round(top + chartH * i / 4.f) + k.px / 2;
        r.DottedLine({ x0 + 8.f, gy }, { x1 - 8.f, gy }, Tint(t, t.pillActive, 0.8f), k.px);
    }
    D2D1_RECT_F inner{ x0 + 2.f, top + 6.f, x1 - 7.f, bottom - 2.f };
    if (ch.b) r.AreaChart(inner, *ch.b, 60, ch.max, ch.cb, ch.cb.WithAlpha(0.20f), ch.cb.WithAlpha(0.f));
    r.AreaChart(inner, *ch.a, 60, ch.max, ch.ca, ch.ca.WithAlpha(0.34f), ch.ca.WithAlpha(0.02f));
    float ey = inner.bottom - (inner.bottom - inner.top) * Clamp01(ch.a->back() / ch.max);
    r.FillEllipse({ inner.right, ey }, 5.f, 5.f, ch.ca.WithAlpha(0.22f));
    r.FillEllipse({ inner.right, ey }, 2.5f, 2.5f, ch.ca);
    return h;
}

// --- Per-core equalizer: one column per physical core, one bar per hardware thread ---------------

struct CoreLayout {
    std::vector<std::vector<int>> groups;
    size_t threads = 1;
    bool mapped = false;
};

CoreLayout Cores(const Stats& st) {
    CoreLayout L;
    int n = (int)st.cores.size();
    L.mapped = st.physicalCores > 0 && (int)st.threadCore.size() >= n;
    for (int i = 0; i < n && L.mapped; ++i)
        if (st.threadCore[i] < 0) L.mapped = false;
    if (L.mapped) {
        L.groups.assign(st.physicalCores, {});
        for (int i = 0; i < n; ++i)
            if (st.threadCore[i] < st.physicalCores) L.groups[st.threadCore[i]].push_back(i);
        L.groups.erase(std::remove_if(L.groups.begin(), L.groups.end(), [](auto& v) { return v.empty(); }), L.groups.end());
    } else {
        for (int i = 0; i < n; ++i) L.groups.push_back({ i });
    }
    for (auto& g : L.groups) L.threads = std::max(L.threads, g.size());
    return L;
}

float CoreBlock(Ctx& k, float y) {
    if (!k.c.coreGrid) return 0;
    CoreLayout L = Cores(k.st);
    if (L.groups.empty()) return 0;
    Renderer& r = k.r;
    size_t perRow = std::min<size_t>(L.groups.size(), 16), rows = (L.groups.size() + perRow - 1) / perRow;
    float T = (float)L.threads, inner = 2.f, ggap = L.threads > 1 ? 6.f : 3.f;
    float bw = std::clamp((k.cw - (perRow - 1) * ggap - perRow * (T - 1) * inner) / (perRow * T), 3.f, 18.f);
    float groupW = T * bw + (T - 1) * inner;
    float capH = LH(r, Font::TipSmall), barH = 30.f, labH = LH(r, Font::TipKey), rowGap = 6.f;
    float h = capH + 6.f + rows * (barH + 2.f + labH) + (rows - 1) * rowGap;
    if (!k.draw) return h;
    const Theme& t = k.t;
    float busiest = 0;
    for (float v : k.st.cores) busiest = std::max(busiest, v);
    r.Text(L.mapped && L.threads > 1 ? L"Per core" : L"Per processor", Font::TipSmallBold,
           { k.x0, y, k.x0 + k.cw, y + capH }, t.subtext);
    r.Text(Fmt(L"busiest %.0f%%", busiest * 100.f), Font::TipSmall, { k.x0, y, k.x0 + k.cw, y + capH }, t.subtext, Align::Right);
    Color track = Color::Lerp(t.pill, t.background, 0.5f);
    bool labelEvery = groupW >= 12.f;
    for (size_t gi = 0; gi < L.groups.size(); ++gi) {
        size_t row = gi / perRow, col = gi % perRow;
        float gx = k.x0 + col * (groupW + ggap), gy = y + capH + 6.f + row * (barH + 2.f + labH + rowGap);
        for (size_t ti = 0; ti < L.groups[gi].size(); ++ti) {
            int lp = L.groups[gi][ti];
            float bx = gx + ti * (bw + inner);
            float rad = std::min(2.5f, bw / 2);
            r.FillRound({ bx, gy, bx + bw, gy + barH }, rad, track);
            float v = lp < (int)k.st.cores.size() ? k.st.cores[lp] : 0.f;
            float fh = std::max(2.f, barH * Clamp01(v));
            r.FillRound({ bx, gy + barH - fh, bx + bw, gy + barH }, rad, LoadColor(t, v, k.c.accent));
        }
        if (labelEvery || gi % 2 == 0)
            r.Text(Fmt(L"%zu", gi + 1), Font::TipKey, { gx - 4.f, gy + barH + 2.f, gx + groupW + 4.f, gy + barH + 2.f + labH },
                   t.subtext, Align::Center);
    }
    return h;
}

// --- Month calendar ---------------------------------------------------------------------------

int DaysIn(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[(m - 1) % 12];
}

SYSTEMTIME AddDays(const SYSTEMTIME& st, int days) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    u.QuadPart += (int64_t)days * 864000000000LL;
    ft = { u.LowPart, u.HighPart };
    SYSTEMTIME out;
    FileTimeToSystemTime(&ft, &out);
    return out;
}

float CalendarBlock(Ctx& k, float y) {
    if (!k.c.calendar) return 0;
    Renderer& r = k.r;
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t buf[96];
    int fdw = 0;  // first day of the week, 0 = Monday
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, buf, _countof(buf))) fdw = _wtoi(buf) % 7;
    SYSTEMTIME first = now;
    first.wDay = 1;
    first.wHour = 12;
    first.wMinute = first.wSecond = first.wMilliseconds = 0;
    first = AddDays(first, 0);  // fills in the weekday
    int firstDow = (first.wDayOfWeek + 6) % 7;
    int days = DaysIn(now.wYear, now.wMonth);
    int lead = (firstDow - fdw + 7) % 7;
    int rows = (lead + days + 6) / 7;
    bool weeks = fdw == 0;  // ISO week numbers line up with Monday-first weeks
    float headH = LH(r, Font::TipSmallBold), dayH = 18.f, cellH = 23.f, wkW = weeks ? 24.f : 0.f;
    float cellW = std::min(40.f, (k.cw - wkW) / 7.f);
    float h = headH + 6.f + dayH + rows * cellH;
    if (!k.draw) return h;
    const Theme& t = k.t;
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_YEARMONTH, &now, nullptr, buf, _countof(buf), nullptr);
    r.Text(buf, Font::TipSmallBold, { k.x0, y, k.x0 + k.cw, y + headH }, t.text);
    float gx = k.x0 + wkW, gy = y + headH + 6.f;
    for (int i = 0; i < 7; ++i) {
        int d = (fdw + i) % 7;  // 0 = Monday
        if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + d, buf, _countof(buf))) buf[0] = 0;
        r.Text(buf, Font::TipKey, { gx + i * cellW, gy, gx + (i + 1) * cellW, gy + dayH }, d >= 5 ? t.subtext.WithAlpha(0.7f) : t.subtext,
               Align::Center);
    }
    gy += dayH;
    for (int row = 0; row < rows; ++row) {
        float cy = gy + row * cellH;
        if (weeks) {
            SYSTEMTIME thu = AddDays(first, row * 7 - lead + 3);
            r.Text(Fmt(L"%d", IsoWeek(thu, nullptr)), Font::TipKey, { k.x0, cy, k.x0 + wkW - 6.f, cy + cellH },
                   t.subtext.WithAlpha(0.55f), Align::Right);
        }
        for (int col = 0; col < 7; ++col) {
            int n = row * 7 + col - lead + 1;
            bool inMonth = n >= 1 && n <= days;
            int shown = inMonth ? n : (n < 1 ? DaysIn(now.wMonth == 1 ? now.wYear - 1 : now.wYear, now.wMonth == 1 ? 12 : now.wMonth - 1) + n : n - days);
            D2D1_RECT_F cell{ gx + col * cellW, cy, gx + (col + 1) * cellW, cy + cellH };
            bool today = inMonth && n == now.wDay;
            bool weekend = (fdw + col) % 7 >= 5;
            Color c = !inMonth ? t.subtext.WithAlpha(0.35f) : weekend ? t.subtext : t.text;
            if (today) {
                float cx = (cell.left + cell.right) / 2, ccy = (cell.top + cell.bottom) / 2;
                r.FillEllipse({ cx, ccy }, 10.5f, 10.5f, k.c.accent);
                c = t.background;
            }
            r.Text(Fmt(L"%d", shown), today ? Font::TipSmallBold : Font::TipSmall, cell, c, Align::Center);
        }
    }
    return h;
}

// --- Meters -----------------------------------------------------------------------------------

float MetersBlock(Ctx& k, float y) {
    const auto& ms = k.c.meters;
    if (ms.empty()) return 0;
    Renderer& r = k.r;
    float lineH = LH(r, Font::Tip), trackH = 6.f, gap = 11.f;
    float h = 0;
    for (size_t i = 0; i < ms.size(); ++i) {
        if (!ms[i].label.empty() || !ms[i].value.empty()) h += lineH + 3.f;
        h += trackH + (i + 1 < ms.size() ? gap : 0.f);
    }
    if (!k.draw) return h;
    const Theme& t = k.t;
    float x1 = k.x0 + k.cw;
    for (const auto& m : ms) {
        if (!m.label.empty() || !m.value.empty()) {
            float vw = r.Measure(m.value, Font::TipSmall);
            r.Text(m.label, Font::Tip, { k.x0, y, x1 - vw - 10.f, y + lineH }, t.text, Align::Left, true);
            r.Text(m.value, Font::TipSmall, { k.x0, y, x1, y + lineH }, t.subtext, Align::Right);
            y += lineH + 3.f;
        }
        r.FillRound({ k.x0, y, x1, y + trackH }, trackH / 2, Color::Lerp(t.pill, t.background, 0.5f));
        float f = Clamp01(m.frac);
        if (f > 0.001f) r.FillRound({ k.x0, y, k.x0 + std::max(trackH, k.cw * f), y + trackH }, trackH / 2, m.color);
        y += trackH + gap;
    }
    return h;
}

// --- Detail rows ------------------------------------------------------------------------------

float RowsLabelW(Renderer& r, const TipContent& c) {
    float w = 0;
    for (auto& ln : c.rows) w = std::max(w, r.Measure(ln.label, Font::Tip));
    return std::ceil(w);
}

float RowsBlock(Ctx& k, float y) {
    const auto& rows = k.c.rows;
    if (rows.empty()) return 0;
    Renderer& r = k.r;
    float lineH = LH(r, Font::Tip) + 4.f;
    float h = rows.size() * lineH;
    if (!k.draw) return h;
    float lw = RowsLabelW(r, k.c), vx = k.x0 + lw + 16.f;
    for (const auto& ln : rows) {
        r.Text(ln.label, Font::Tip, { k.x0, y, vx, y + lineH }, k.t.subtext, Align::Left, true);
        r.Text(ln.value, Font::Tip, { vx, y, k.x0 + k.cw, y + lineH }, k.t.text, Align::Left, true);
        y += lineH;
    }
    return h;
}

// --- Usage hints: keycap + what it does -------------------------------------------------------

constexpr float kHintGap = 14.f, kKeyH = 18.f;

float HintWidth(Renderer& r, const TipHint& h) {
    return std::ceil(r.Measure(h.key, Font::TipKey) + 12.f + 6.f + r.Measure(h.action, Font::TipSmall));
}

float HintsBlock(Ctx& k, float y) {
    const auto& hints = k.c.hints;
    if (hints.empty()) return 0;
    Renderer& r = k.r;
    // Flow left to right, wrapping when a hint doesn't fit.
    int lines = 1;
    float x = 0;
    for (auto& hn : hints) {
        float w = HintWidth(r, hn);
        if (x > 0 && x + w > k.cw) { ++lines; x = 0; }
        x += w + kHintGap;
    }
    float h = 11.f + lines * kKeyH + (lines - 1) * 6.f;
    if (!k.draw) return h;
    const Theme& t = k.t;
    r.FillRect({ k.x0, y, k.x0 + k.cw, y + k.px }, Tint(t, t.pillActive, 0.6f));
    float yy = y + 11.f;
    x = k.x0;
    for (auto& hn : hints) {
        float w = HintWidth(r, hn);
        if (x > k.x0 && x + w > k.x0 + k.cw) { x = k.x0; yy += kKeyH + 6.f; }
        float kw = std::ceil(r.Measure(hn.key, Font::TipKey) + 12.f);
        D2D1_RECT_F key{ x, yy, x + kw, yy + kKeyH };
        r.FillRound(key, 5.f, Tint(t, t.background, 0.55f));  // darker lip at the bottom, like a keycap
        r.FillRound({ key.left, key.top, key.right, key.bottom - 1.5f }, 5.f, Tint(t, t.pillActive, 0.75f));
        r.Text(hn.key, Font::TipKey, { key.left, key.top, key.right, key.bottom - 1.f }, t.text, Align::Center);
        r.Text(hn.action, Font::TipSmall, { key.right + 6.f, yy, k.x0 + k.cw, yy + kKeyH }, t.subtext, Align::Left, true);
        x += w + kHintGap;
    }
    return h;
}

// --- Whole card -------------------------------------------------------------------------------

float NaturalWidth(Renderer& r, const TipContent& c, const Stats& st) {
    float w = kMinW - 2 * kPad;
    float badge = c.badge.empty() ? 0.f : r.Measure(c.badge, Font::TipKey) + 26.f;
    w = std::max(w, kChip + 11.f + std::min(r.Measure(c.title, Font::TipBold), 320.f) + badge);
    if (!c.subtitle.empty()) w = std::max(w, kChip + 11.f + std::min(r.Measure(c.subtitle, Font::TipSmall), 320.f));
    if (!c.stats.empty()) {
        float sw = 0;
        for (auto& s : c.stats) sw += StatWidth(r, s);
        w = std::max(w, sw + (c.stats.size() - 1) * kStatGap);
    }
    if (c.hasChart) w = std::max(w, 288.f);
    if (c.coreGrid) {
        CoreLayout L = Cores(st);
        size_t perRow = std::min<size_t>(L.groups.size(), 16);
        float T = (float)L.threads, ggap = L.threads > 1 ? 6.f : 3.f;
        w = std::max(w, perRow * (T * 9.f + (T - 1) * 2.f) + (perRow - 1) * ggap);
    }
    if (c.calendar) w = std::max(w, 7 * 31.f + 24.f);
    if (!c.meters.empty()) {
        w = std::max(w, 260.f);
        for (auto& m : c.meters) w = std::max(w, r.Measure(m.label, Font::Tip) + 24.f + r.Measure(m.value, Font::TipSmall));
    }
    if (!c.rows.empty()) {
        float vw = 0;
        for (auto& ln : c.rows) vw = std::max(vw, r.Measure(ln.value, Font::Tip));
        w = std::max(w, RowsLabelW(r, c) + 16.f + vw);
    }
    float hw = 0;
    for (auto& h : c.hints) hw += HintWidth(r, h) + kHintGap;
    if (hw > 0) w = std::max(w, std::min(hw - kHintGap, kMaxW - 2 * kPad));
    return std::clamp(std::ceil(w) + 2 * kPad, kMinW, kMaxW);
}

float Content(Ctx& k) {
    float y = kPad;
    y += Header(k, y);
    float h;
    if ((h = StatsBlock(k, y + kSection + 2.f)) > 0) y += kSection + 2.f + h;
    if ((h = ChartBlock(k, y + kSection)) > 0) y += kSection + h;
    if ((h = CoreBlock(k, y + kSection)) > 0) y += kSection + h;
    if ((h = CalendarBlock(k, y + kSection)) > 0) y += kSection + h;
    if ((h = MetersBlock(k, y + kSection)) > 0) y += kSection + h;
    if ((h = RowsBlock(k, y + kSection - 2.f)) > 0) y += kSection - 2.f + h;
    if ((h = HintsBlock(k, y + kSection)) > 0) y += kSection + h;
    return y + kPad;
}

// Signed distance from a pixel centre to a rounded rectangle (negative inside).
float SdRoundRect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

bool Tooltip::Create(HINSTANCE inst) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = Proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TopBarTip";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                           L"TopBarTip", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, this);
    if (!hwnd) return false;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
    BOOL yes = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_DONOTROUND;  // the card draws its own corners
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    return true;
}

void Tooltip::Destroy() {
    if (hwnd) {
        KillTimer(hwnd, T_ANIM);
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (memDC_) {
        if (oldBmp_) SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    dibW_ = dibH_ = 0;
    maskW_ = maskH_ = 0;
    visible_ = false;
    owner_ = nullptr;
}

void Tooltip::EnsureSurface(int w, int h) {
    if (!memDC_) {
        HDC screen = GetDC(nullptr);
        memDC_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    if (dib_ && dibW_ == w && dibH_ == h) return;
    if (oldBmp_) { SelectObject(memDC_, oldBmp_); oldBmp_ = nullptr; }
    if (dib_) DeleteObject(dib_);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bits_ = (uint32_t*)bits;
    oldBmp_ = dib_ ? SelectObject(memDC_, dib_) : nullptr;
    dibW_ = dib_ ? w : 0;
    dibH_ = dib_ ? h : 0;
}

// Coverage of the card (anti-aliased rounded rectangle) and the shadow around it: a wide, soft
// ambient shadow plus a tight contact shadow, like Windows 11 flyouts.
void Tooltip::BuildMask() {
    int W = dibW_, H = dibH_;
    float s = maskScale_ = owner_ ? owner_->Scale() : 1.f;
    cover_.assign((size_t)W * H, 0);
    shadow_.assign((size_t)W * H, 0);
    float hw = cardW_ / 2.f, hh = cardH_ / 2.f, cx = cardX_ + hw, cy = cardY_ + hh, rad = kRadius * s;
    const float o1 = 6.f * s, g1 = 10.f * s * 1.41421356f, a1 = 0.38f;
    const float o2 = 1.5f * s, g2 = 2.f * s * 1.41421356f, a2 = 0.32f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float cov = Clamp01(0.5f - SdRoundRect(px, py, cx, cy, hw, hh, rad));
            float sh = 0;
            if (cov < 1.f) {
                float s1 = a1 * 0.5f * std::erfc(SdRoundRect(px, py - o1, cx, cy, hw, hh, rad) / g1);
                float s2 = a2 * 0.5f * std::erfc(SdRoundRect(px, py - o2, cx, cy, hw, hh, rad) / g2);
                sh = 1.f - (1.f - s1) * (1.f - s2);
            }
            size_t i = (size_t)y * W + x;
            cover_[i] = (uint8_t)std::lround(cov * 255.f);
            shadow_[i] = (uint8_t)std::lround(Clamp01(sh) * 255.f);
        }
    }
    maskW_ = W;
    maskH_ = H;
    maskCardW_ = cardW_;
    maskCardH_ = cardH_;
}

bool Tooltip::Compose() {
    App& a = *App::I;
    if (!owner_) return false;
    Bar& bar = *owner_;
    TipContent c;
    a.BuildTooltip(hit_, c);
    if (c.Empty()) return false;
    Renderer& r = a.renderer;
    const Theme& t = a.cfg.theme;
    float s = bar.Scale();

    float w = NaturalWidth(r, c, a.stats);
    if (hit_.mod == widthMod_ && hit_.hwnd == widthHwnd_) w = std::max(w, minW_);
    widthMod_ = hit_.mod;
    widthHwnd_ = hit_.hwnd;
    minW_ = w;
    Ctx k{ r, t, a.stats, c, kPad, w - 2 * kPad, 1.f / s, false };
    float h = Content(k);

    int mx = (int)std::lround(kMarginX * s), mt = (int)std::lround(kMarginTop * s), mb = (int)std::lround(kMarginBottom * s);
    cardW_ = (int)std::ceil(w * s);
    cardH_ = (int)std::ceil(h * s);
    cardX_ = mx;
    cardY_ = mt;
    EnsureSurface(cardW_ + 2 * mx, cardH_ + mt + mb);
    if (!bits_) return false;
    if (maskW_ != dibW_ || maskH_ != dibH_ || maskScale_ != s || maskCardW_ != cardW_ || maskCardH_ != cardH_) BuildMask();

    if (!r.Begin(memDC_, dibW_, dibH_, (float)bar.mon.dpi)) return false;
    r.Clear(t.pill);
    float W = cardW_ / s, H = cardH_ / s;
    r.SetTransform(D2D1::Matrix3x2F::Translation(cardX_ / s, cardY_ / s));
    // A faint wash of the module's colour behind the header, and an edge that is lit from above.
    r.FillRectGradientV({ 0, 0, W, 60.f }, Tint(t, c.accent, 0.06f), t.pill);
    {
        D2D1_GRADIENT_STOP stops[2] = { { 0.f, Color::Lerp(t.pill, t.text, 0.20f).d2d() },
                                        { 1.f, Color::Lerp(t.pill, t.pillHover, 0.7f).d2d() } };
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> edge;
        ID2D1RenderTarget* rt = r.RT();
        float e = 0.5f / s;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &coll)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, H }), coll.Get(), &edge)))
            rt->DrawRoundedRectangle(D2D1::RoundedRect({ e, e, W - e, H - e }, kRadius - e, kRadius - e), edge.Get(), 1.f / s);
    }
    k.draw = true;
    k.cw = W - 2 * kPad;
    Content(k);
    r.ResetTransform();
    r.End();
    GdiFlush();

    // Card pixels become opaque, corner pixels partly, and the shadow is black at its own alpha.
    const size_t n = (size_t)dibW_ * dibH_;
    for (size_t i = 0; i < n; ++i) {
        uint32_t cv = cover_[i];
        uint32_t p = bits_[i];
        if (cv == 255) { bits_[i] = p | 0xFF000000u; continue; }
        uint32_t al = cv + (shadow_[i] * (255 - cv) + 127) / 255;
        uint32_t rr = (((p >> 16) & 255) * cv + 127) / 255, gg = (((p >> 8) & 255) * cv + 127) / 255, bb = ((p & 255) * cv + 127) / 255;
        bits_[i] = (al << 24) | (rr << 16) | (gg << 8) | bb;
    }
    return true;
}

void Tooltip::Present(bool newContent) {
    POINT pos{ shown_.x - cardX_, shown_.y - cardY_ }, src{ 0, 0 };
    SIZE size{ dibW_, dibH_ };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)std::lround(Clamp01(alpha_) * 255.f), AC_SRC_ALPHA };
    if (newContent) UpdateLayeredWindow(hwnd, nullptr, &pos, &size, memDC_, &src, 0, &bf, ULW_ALPHA);
    else UpdateLayeredWindow(hwnd, nullptr, &pos, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

void Tooltip::Show(Bar* bar, const Hit& hit) {
    if (!hwnd || !bar) return;
    bool wasOpen = visible_ && owner_ == bar;
    bool sameItem = wasOpen && hit.mod == hit_.mod && hit.hwnd == hit_.hwnd;
    bool closing = anim_ == Anim::Close;
    owner_ = bar;
    hit_ = hit;
    visible_ = true;
    if (hit.mod == Mod::Cpu) App::I->stats.SampleCpuCountersIfStale();
    if (!Compose()) { Hide(-1); return; }

    // Centred under the module, kept on the bar's monitor.
    float s = bar->Scale();
    int margin = (int)std::lround(6 * s);
    int cx = bar->barRect.left + (int)(((hit.rect.left + hit.rect.right) / 2) * s);
    target_.x = std::clamp(cx - cardW_ / 2, (int)bar->mon.rect.left + margin, (int)bar->mon.rect.right - cardW_ - margin);
    target_.y = bar->barRect.bottom + margin;

    double now = NowSeconds();
    if (!App::I->Animations()) {
        anim_ = Anim::None;
        shown_ = target_;
        alpha_ = 1.f;
    } else if (wasOpen && !closing) {
        if (!sameItem) {  // glide over to the newly hovered module
            from_ = shown_;
            fromAlpha_ = alpha_;
            anim_ = Anim::Glide;
            animStart_ = now;
        }
    } else {
        // Drop in from just under the bar (or pick up a card that was fading away).
        from_ = closing ? POINT{ target_.x, shown_.y } : POINT{ target_.x, target_.y - (int)std::lround(8 * s) };
        fromAlpha_ = closing ? alpha_ : 0.f;
        shown_ = from_;
        alpha_ = fromAlpha_;
        anim_ = Anim::Open;
        animStart_ = now;
    }
    if (anim_ == Anim::None) shown_ = target_;
    Present(true);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (anim_ != Anim::None) SetTimer(hwnd, T_ANIM, 10, nullptr);
}

void Tooltip::Refresh() {
    if (!visible_ || !owner_) return;
    if (!Compose()) { Hide(-1); return; }
    // The card can grow: keep it centred under its module.
    Bar& bar = *owner_;
    float s = bar.Scale();
    int margin = (int)std::lround(6 * s);
    int cx = bar.barRect.left + (int)(((hit_.rect.left + hit_.rect.right) / 2) * s);
    target_.x = std::clamp(cx - cardW_ / 2, (int)bar.mon.rect.left + margin, (int)bar.mon.rect.right - cardW_ - margin);
    if (anim_ == Anim::None) shown_ = target_;
    Present(true);
}

void Tooltip::Tick() {
    double now = NowSeconds();
    auto lerp = [](int a, int b, float t) { return a + (int)std::lround((b - a) * t); };
    float t = 1.f;
    switch (anim_) {
    case Anim::Open: {
        t = (float)((now - animStart_) / kOpenSeconds);
        float e = EaseOut(t);
        shown_ = { target_.x, lerp(from_.y, target_.y, e) };
        alpha_ = fromAlpha_ + (1.f - fromAlpha_) * Clamp01(t * 1.5f);
        break;
    }
    case Anim::Glide: {
        t = (float)((now - animStart_) / kGlideSeconds);
        float e = EaseOut(t);
        shown_ = { lerp(from_.x, target_.x, e), lerp(from_.y, target_.y, e) };
        alpha_ = fromAlpha_ + (1.f - fromAlpha_) * e;
        break;
    }
    case Anim::Close: {
        t = (float)((now - animStart_) / kCloseSeconds);
        alpha_ = fromAlpha_ * (1.f - Clamp01(t));
        shown_ = { from_.x, from_.y - (int)std::lround(4.f * maskScale_ * Clamp01(t)) };
        break;
    }
    case Anim::None:
        break;
    }
    if (t >= 1.f) {
        KillTimer(hwnd, T_ANIM);
        if (anim_ == Anim::Close) {
            anim_ = Anim::None;
            ShowWindow(hwnd, SW_HIDE);
            return;
        }
        anim_ = Anim::None;
        shown_ = target_;
        alpha_ = 1.f;
    }
    Present(false);
}

void Tooltip::Hide(int line) {
    if (!hwnd) return;
    KillTimer(hwnd, T_ANIM);
    if (visible_ || anim_ == Anim::Close) {
        ShowWindow(hwnd, SW_HIDE);
        DebugLog(L"tip hide line %d", line);
    }
    anim_ = Anim::None;
    visible_ = false;
    owner_ = nullptr;
    widthMod_ = Mod::None;
    alpha_ = 0;
}

void Tooltip::FadeOut() {
    if (!hwnd || !visible_) return;
    if (!App::I->Animations()) { Hide(__LINE__); return; }
    visible_ = false;
    owner_ = nullptr;
    widthMod_ = Mod::None;
    from_ = shown_;
    fromAlpha_ = alpha_;
    anim_ = Anim::Close;
    animStart_ = NowSeconds();
    SetTimer(hwnd, T_ANIM, 10, nullptr);
}

LRESULT CALLBACK Tooltip::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = (Tooltip*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_TIMER:
        if (self && w == T_ANIM) self->Tick();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

#ifdef TOPBAR_DRY_RUN
#include <wincodec.h>
// Test builds: every module's card on a dark backdrop, saved as PNG (for checking the design and
// for the README pictures).
void Tooltip::Snapshot(Bar* bar, const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    std::vector<Hit> hits = bar->hits;
    int index = 0;
    for (const Hit& hit : hits) {
        owner_ = bar;
        hit_ = hit;
        widthMod_ = Mod::None;
        if (hit.mod == Mod::Cpu) {  // clock speed needs two samples
            App::I->stats.SetCpuCountersActive(true);
            Sleep(600);
            App::I->stats.SampleCpuCountersIfStale();
        }
        if (!Compose()) continue;
        std::vector<uint32_t> out((size_t)dibW_ * dibH_);
        const Theme& t = App::I->cfg.theme;
        Color b = Color::Lerp(t.background, t.pill, 0.35f);
        const uint32_t bg = ((uint32_t)std::lround(b.r * 255) << 16) | ((uint32_t)std::lround(b.g * 255) << 8) | (uint32_t)std::lround(b.b * 255);
        for (size_t i = 0; i < out.size(); ++i) {
            uint32_t p = bits_[i], ia = 255 - (p >> 24);
            auto ch = [&](int sh) { return (((p >> sh) & 255) + (((bg >> sh) & 255) * ia + 127) / 255) & 255; };
            out[i] = 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
        std::wstring file = dir + Fmt(L"\\tip-%02d-%d.png", index++, (int)hit.mod);
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
            FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
            FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(dibW_, dibH_)) || FAILED(frame->SetPixelFormat(&fmt)) ||
            FAILED(frame->WritePixels(dibH_, dibW_ * 4, (UINT)(out.size() * 4), (BYTE*)out.data())) ||
            FAILED(frame->Commit()) || FAILED(enc->Commit()))
            DebugLog(L"snapshot: could not write %s", file.c_str());
    }
    owner_ = nullptr;
    visible_ = false;
    widthMod_ = Mod::None;
}
#endif
