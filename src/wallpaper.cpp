#include "wallpaper.h"
#include "config.h"
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

namespace {

constexpr Color Theme::* kThemeColors[] = {
    &Theme::background, &Theme::pill, &Theme::pillHover, &Theme::pillActive, &Theme::text, &Theme::subtext,
    &Theme::accent, &Theme::cpu, &Theme::gpu, &Theme::memory, &Theme::disk, &Theme::temp, &Theme::hot,
    &Theme::network, &Theme::volume, &Theme::bluetooth, &Theme::battery, &Theme::warn, &Theme::power, &Theme::clock,
};
static_assert(sizeof(kThemeColors) / sizeof(kThemeColors[0]) * sizeof(Color) == sizeof(Theme),
              "every Theme colour must be listed");

// --- OKLab (Björn Ottosson): a perceptual colour space, so "same lightness" really looks the same.

struct Lab { float L, a, b; };

float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float ToGamma(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f; }

Lab ToLab(float r, float g, float b) {
    float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return { 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
             1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
             0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s };
}

// False when the colour falls outside sRGB (the result is clamped).
bool FromLab(const Lab& c, Color& out) {
    float l = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    l = l * l * l; m = m * m * m; s = s * s * s;
    float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    const float e = 0.0005f;
    bool in = r >= -e && r <= 1 + e && g >= -e && g <= 1 + e && b >= -e && b <= 1 + e;
    out = { ToGamma(Clamp01(r)), ToGamma(Clamp01(g)), ToGamma(Clamp01(b)), 1.f };
    return in;
}

// Lightness, chroma, hue (radians). Chroma is reduced until the colour fits in sRGB.
Color Oklch(float L, float C, float h) {
    Color out;
    if (FromLab({ L, C * std::cos(h), C * std::sin(h) }, out)) return out;
    float lo = 0, hi = C;
    for (int i = 0; i < 18; ++i) {
        float mid = (lo + hi) / 2;
        if (FromLab({ L, mid * std::cos(h), mid * std::sin(h) }, out)) lo = mid;
        else hi = mid;
    }
    FromLab({ L, lo * std::cos(h), lo * std::sin(h) }, out);
    return out;
}

struct Cluster { Lab c{}; float w = 0; };

float Dist2(const Lab& x, const Lab& y) {
    float dl = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
    return dl * dl + da * da + db * db;
}

// k-means in OKLab, seeded deterministically (same wallpaper -> same palette).
std::vector<Cluster> KMeans(const std::vector<Lab>& px, int k) {
    std::vector<Cluster> cl;
    if (px.empty()) return cl;
    std::vector<float> best(px.size(), 1e9f);
    cl.push_back({ px[px.size() / 2], 0 });
    while ((int)cl.size() < k) {
        size_t farthest = 0;
        float farD = -1.f;
        for (size_t i = 0; i < px.size(); ++i) {
            best[i] = std::min(best[i], Dist2(px[i], cl.back().c));
            if (best[i] > farD) { farD = best[i]; farthest = i; }
        }
        if (farD < 1e-5f) break;  // fewer distinct colours than k
        cl.push_back({ px[farthest], 0 });
    }
    std::vector<int> owner(px.size(), 0);
    for (int iter = 0; iter < 12; ++iter) {
        bool moved = false;
        for (size_t i = 0; i < px.size(); ++i) {
            int bi = 0;
            float bd = 1e9f;
            for (int j = 0; j < (int)cl.size(); ++j) {
                float d = Dist2(px[i], cl[j].c);
                if (d < bd) { bd = d; bi = j; }
            }
            if (owner[i] != bi) { owner[i] = bi; moved = true; }
        }
        std::vector<Lab> sum(cl.size(), Lab{ 0, 0, 0 });
        std::vector<float> n(cl.size(), 0);
        for (size_t i = 0; i < px.size(); ++i) {
            Lab& s = sum[owner[i]];
            s.L += px[i].L; s.a += px[i].a; s.b += px[i].b;
            n[owner[i]] += 1;
        }
        for (size_t j = 0; j < cl.size(); ++j) {
            if (n[j] > 0) cl[j].c = { sum[j].L / n[j], sum[j].a / n[j], sum[j].b / n[j] };
            cl[j].w = n[j] / (float)px.size();
        }
        if (!moved && iter > 0) break;
    }
    return cl;
}

std::wstring ThemesDir() {
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) dir = appdata;
    if (appdata) CoTaskMemFree(appdata);
    return dir.empty() ? dir : dir + L"\\Microsoft\\Windows\\Themes";
}

// A picture we can read without touching the network or downloading a cloud-only file.
bool UsableLocal(const std::wstring& p) {
    if (p.size() < 4 || !iswalpha(p[0]) || p[1] != L':') return false;
    wchar_t root[4] = { p[0], L':', L'\\', 0 };
    if (GetDriveTypeW(root) == DRIVE_REMOTE) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return !(a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
}

void AddFileStamp(std::wstring& sig, const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!p.empty() && GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        sig += Fmt(L"|%08lx%08lx:%lu", d.ftLastWriteTime.dwHighDateTime, d.ftLastWriteTime.dwLowDateTime, d.nFileSizeLow);
}

}  // namespace

std::wstring WallpaperSignature() {
    wchar_t path[MAX_PATH * 2] = L"";
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, _countof(path), path, 0);
    std::wstring sig = path;
    AddFileStamp(sig, path);
    // Windows keeps a copy of the current picture (one per monitor when they differ) in Themes.
    std::wstring themes = ThemesDir();
    if (!themes.empty()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((themes + L"\\Transcoded*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                sig += Fmt(L"|%s:%08lx%08lx", fd.cFileName, fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    sig += Fmt(L"|%06lx", (unsigned long)GetSysColor(COLOR_DESKTOP));
    return sig;
}

WallpaperSource CurrentWallpaper() {
    WallpaperSource s;
    s.solid = GetSysColor(COLOR_DESKTOP);
    ComPtr<IDesktopWallpaper> dw;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        UINT n = 0;
        dw->GetMonitorDevicePathCount(&n);
        for (UINT i = 0; i < n && s.path.empty(); ++i) {
            LPWSTR id = nullptr;
            if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
            RECT rc{};
            LPWSTR wp = nullptr;
            if (SUCCEEDED(dw->GetMonitorRECT(id, &rc)) && EqualRect(&rc, &mi.rcMonitor) &&
                SUCCEEDED(dw->GetWallpaper(id, &wp)) && wp)
                s.path = wp;
            if (wp) CoTaskMemFree(wp);
            CoTaskMemFree(id);
        }
        COLORREF c = 0;
        if (SUCCEEDED(dw->GetBackgroundColor(&c))) s.solid = c;
    }
    std::wstring transcoded = ThemesDir() + L"\\TranscodedWallpaper";
    if (UsableLocal(transcoded)) s.fallback = transcoded;
    if (!UsableLocal(s.path)) {
        wchar_t spi[MAX_PATH * 2] = L"";
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, _countof(spi), spi, 0);
        if (!s.fallback.empty()) s.path = s.fallback;
        else if (UsableLocal(spi)) s.path = spi;
        else s.path.clear();
    }
    if (s.fallback == s.path) s.fallback.clear();
    return s;
}

namespace {
// A small copy of the picture (at most 160 px on its long side) as OKLab colours.
bool SamplePicture(const std::wstring& path, std::vector<Lab>& px) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.nFileSizeHigh > 0) return false;  // over 4 GB: not a wallpaper we want to read
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    UINT W = 0, H = 0;
    if (FAILED(frame->GetSize(&W, &H)) || !W || !H || (uint64_t)W * H > 400ull * 1000 * 1000) return false;
    float k = std::min(1.f, 160.f / (float)std::max(W, H));
    UINT tw = std::max(1u, (UINT)std::lround(W * k)), th = std::max(1u, (UINT)std::lround(H * k));
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICFormatConverter> conv;
    std::vector<uint32_t> buf((size_t)tw * th);
    if (FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(sc.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.f,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->CopyPixels(nullptr, tw * 4, (UINT)(buf.size() * 4), (BYTE*)buf.data())))
        return false;
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = ToLinear(i / 255.f);
    px.clear();
    px.reserve(buf.size());
    for (uint32_t p : buf) {
        if ((p >> 24) < 128) continue;
        px.push_back(ToLab(lut[(p >> 16) & 255], lut[(p >> 8) & 255], lut[p & 255]));
    }
    return !px.empty();
}
}  // namespace

bool ThemeFromWallpaper(const WallpaperSource& src, const Theme& base, float tint, Theme& out) {
    std::vector<Lab> px;
    if (src.path.empty()) {
        float r = ToLinear(GetRValue(src.solid) / 255.f), g = ToLinear(GetGValue(src.solid) / 255.f),
              b = ToLinear(GetBValue(src.solid) / 255.f);
        px.assign(64, ToLab(r, g, b));
    } else if (!SamplePicture(src.path, px) && !SamplePicture(src.fallback, px)) {
        return false;  // e.g. a format whose add-on codec isn't available: keep the current colours
    }

    std::vector<Cluster> cl = KMeans(px, 8);
    if (cl.empty()) return false;
    const Cluster* dom = &cl[0];
    for (auto& c : cl) if (c.w > dom->w) dom = &c;

    out = base;
    // Accents from the picture's vivid colours, best first (vivid and plentiful), like wallust.
    struct Hue { float h, c, score; };
    std::vector<Hue> hues;
    for (auto& c : cl) {
        float C = std::hypot(c.c.a, c.c.b);
        if (C < 0.035f || c.c.L < 0.2f || c.c.L > 0.97f || c.w < 0.004f) continue;
        hues.push_back({ std::atan2(c.c.b, c.c.a), C, C * std::pow(c.w, 0.4f) });
    }
    std::sort(hues.begin(), hues.end(), [](const Hue& x, const Hue& y) { return x.score > y.score; });
    const float deg = 3.14159265f / 180.f;
    for (size_t i = 0; i < hues.size(); ++i)
        for (size_t j = i + 1; j < hues.size();) {
            float dh = std::fabs(std::remainder(hues[i].h - hues[j].h, 2 * 3.14159265f));
            if (dh < 20 * deg) hues.erase(hues.begin() + j);
            else ++j;
        }

    // Neutrals (background, pills, text) stay on the default palette's lightness ladder, so they are
    // as readable, and take the picture's main colour. When that colour is greyish (clouds, fog),
    // the most vivid colour is used instead. `tint` (0..1) sets how strongly.
    float tintC = std::hypot(dom->c.a, dom->c.b), tintH = std::atan2(dom->c.b, dom->c.a);
    if (tintC < 0.05f && !hues.empty()) {
        tintH = hues[0].h;
        tintC = std::max(tintC, hues[0].c * 0.6f);
    }
    float nc = std::min(tintC * 0.9f, 0.075f * Clamp01(tint));
    auto neutral = [&](float L, float k) { return Oklch(L, nc * k, tintH); };
    out.background = neutral(0.18f, 0.85f);
    out.pill = neutral(0.245f, 1.f);
    out.pillHover = neutral(0.325f, 0.95f);
    out.pillActive = neutral(0.40f, 0.85f);
    out.text = neutral(0.90f, 0.45f);
    out.subtext = neutral(0.65f, 0.75f);
    out.clock = out.text;

    if (hues.empty()) return true;  // a grey picture: grey neutrals, the usual accents
    // Too few colours to tell modules apart: add close neighbours of the main one.
    const float offsets[] = { 30, -30, 60, -60 };
    for (size_t i = 0; hues.size() < 4 && i < std::size(offsets); ++i)
        hues.push_back({ hues[0].h + offsets[i] * deg, hues[0].c * 0.9f, 0 });
    auto vivid = [&](size_t i, float L) {
        const Hue& u = hues[i % hues.size()];
        return Oklch(L, std::clamp(u.c * 1.1f, 0.08f, 0.15f), u.h);
    };
    out.accent = vivid(0, 0.79f);
    Color* mods[] = { &out.cpu, &out.gpu, &out.memory, &out.disk, &out.network, &out.volume, &out.bluetooth, &out.battery };
    for (size_t i = 0; i < std::size(mods); ++i) *mods[i] = vivid(i, i % 2 ? 0.84f : 0.79f);
    // Red / yellow / orange keep their meaning (hot, warning, temperature) and the power colour.
    return true;
}

Theme LerpTheme(const Theme& a, const Theme& b, float t) {
    Theme r = a;
    for (auto m : kThemeColors) r.*m = Color::Lerp(a.*m, b.*m, t);
    return r;
}
