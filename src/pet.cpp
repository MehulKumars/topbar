#include "pet.h"
#include "render.h"
#include "config.h"
#include <objbase.h>

namespace {

// Limits for user-supplied images. Generous for any real desktop-pet GIF, small enough that a
// broken or hostile file can't use much memory or time (decoding runs on a background thread).
constexpr uint64_t kMaxFileBytes = 32ull << 20;    // 32 MB on disk
constexpr UINT kMaxSide = 2048;                    // logical screen / frame size
constexpr UINT kMaxFrames = 600;
constexpr uint64_t kMaxWork = 160ull << 20;        // canvas pixels x frames we composite
constexpr uint64_t kMaxSpriteBytes = 48ull << 20;  // the scaled frames we keep

bool IsLocalPath(const std::wstring& p, std::wstring& err) {
    // Drive-letter paths only: no \\server\share, \\?\ or device paths. Opening a network path can
    // hand the user's Windows credentials to that server, and is slow anyway.
    if (p.size() < 4 || !iswalpha(p[0]) || p[1] != L':' || (p[2] != L'\\' && p[2] != L'/')) {
        err = L"Only files on this PC can be used (no network or device paths).";
        return false;
    }
    wchar_t root[4] = { p[0], L':', L'\\', 0 };
    UINT type = GetDriveTypeW(root);
    if (type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) {
        err = L"Files on network drives can't be used.";
        return false;
    }
    return true;
}

bool ReadLocalFile(const std::wstring& p, std::vector<uint8_t>& out, std::wstring& err) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"The file can't be opened."; return false; }
    bool ok = GetFileType(h) == FILE_TYPE_DISK;
    // A link inside a local folder could still point at a network share.
    wchar_t final[MAX_PATH * 2];
    DWORD n = GetFinalPathNameByHandleW(h, final, _countof(final), FILE_NAME_NORMALIZED);
    if (ok && n > 0 && n < _countof(final) && _wcsnicmp(final, L"\\\\?\\UNC\\", 8) == 0) ok = false;
    LARGE_INTEGER size{};
    if (ok) ok = GetFileSizeEx(h, &size) && size.QuadPart > 0;
    if (ok && (uint64_t)size.QuadPart > kMaxFileBytes) {
        CloseHandle(h);
        err = L"The file is over 32 MB.";
        return false;
    }
    if (!ok) { CloseHandle(h); err = L"Only regular files on this PC can be used."; return false; }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size();
    CloseHandle(h);
    if (!ok) err = L"The file can't be read.";
    return ok;
}

UINT MetaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def) {
    if (!q) return def;
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT r = def;
    if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) r = v.bVal;
        else if (v.vt == VT_UI2) r = v.uiVal;
        else if (v.vt == VT_UI4) r = v.ulVal;
    }
    PropVariantClear(&v);
    return r;
}

// Premultiplied source-over.
inline uint32_t Over(uint32_t s, uint32_t d) {
    uint32_t ia = 255 - (s >> 24);
    auto ch = [&](int sh) { return (((s >> sh) & 255) + (((d >> sh) & 255) * ia + 127) / 255) & 255; };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Cubic filters can ring past the alpha edge; keep every pixel valid premultiplied colour.
void ClampPremultiplied(std::vector<uint32_t>& px) {
    for (auto& p : px) {
        uint32_t a = p >> 24, r = std::min((p >> 16) & 255, a), g = std::min((p >> 8) & 255, a), b = std::min(p & 255, a);
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

}  // namespace

bool LoadPetSprite(const std::wstring& path, int heightPx, int smooth, PetSprite& out, std::wstring& error) {
    out = PetSprite{};
    if (!IsLocalPath(path, error)) return false;
    std::vector<uint8_t> data;
    if (!ReadLocalFile(path, data, error)) return false;

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(data.data(), (DWORD)data.size())) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
        error = L"Not an image Windows can read.";
        return false;
    }
    UINT count = 0;
    ComPtr<IWICBitmapFrameDecode> first;
    if (FAILED(dec->GetFrameCount(&count)) || count == 0 || FAILED(dec->GetFrame(0, &first))) {
        error = L"The image has no frames.";
        return false;
    }
    count = std::min(count, kMaxFrames);
    GUID container{};
    dec->GetContainerFormat(&container);
    bool gif = container == GUID_ContainerFormatGif;

    // A GIF's frames are patches on a "logical screen"; anything else is one picture.
    UINT W = 0, H = 0;
    if (gif) {
        ComPtr<IWICMetadataQueryReader> q;
        if (SUCCEEDED(dec->GetMetadataQueryReader(&q))) {
            W = MetaUInt(q.Get(), L"/logscrdesc/Width", 0);
            H = MetaUInt(q.Get(), L"/logscrdesc/Height", 0);
        }
    }
    if (!W || !H) first->GetSize(&W, &H);
    if (!W || !H || W > kMaxSide || H > kMaxSide) {
        error = L"The image is larger than 2048 x 2048.";
        return false;
    }
    int th = std::max(1, heightPx);
    // Enlarging keeps pixel art crisp; shrinking averages for a clean small picture.
    bool enlarge = th >= (int)H;
    WICBitmapInterpolationMode mode =
        enlarge ? (smooth == 1 ? WICBitmapInterpolationModeHighQualityCubic : WICBitmapInterpolationModeNearestNeighbor)
                : (smooth == 0 ? WICBitmapInterpolationModeNearestNeighbor : WICBitmapInterpolationModeFant);
    // Crisp pixels need a whole-number scale (1.5x would make every other pixel fat).
    if (enlarge && mode == WICBitmapInterpolationModeNearestNeighbor) th = (int)H * std::max(1, th / (int)H);
    int tw = std::max(1, (int)std::lround((double)W * th / H));
    if (tw > th * 8) { error = L"The image is too wide for the bar."; return false; }
    uint64_t canvasPx = (uint64_t)W * H;
    count = (UINT)std::min<uint64_t>(count, std::max<uint64_t>(1, kMaxWork / canvasPx));
    count = (UINT)std::min<uint64_t>(count, std::max<uint64_t>(1, kMaxSpriteBytes / ((uint64_t)tw * th * 4)));

    std::vector<uint32_t> canvas((size_t)canvasPx, 0), saved, patch;
    UINT prevDisposal = 0;
    RECT prev{};
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> fr;
        if (i == 0) fr = first;
        else if (FAILED(dec->GetFrame(i, &fr))) break;
        UINT fw = 0, fh = 0;
        fr->GetSize(&fw, &fh);
        if (!fw || !fh || fw > kMaxSide || fh > kMaxSide) break;
        UINT left = 0, top = 0, delay = 10, disposal = 0;
        if (gif) {
            ComPtr<IWICMetadataQueryReader> q;
            if (SUCCEEDED(fr->GetMetadataQueryReader(&q))) {
                left = MetaUInt(q.Get(), L"/imgdesc/Left", 0);
                top = MetaUInt(q.Get(), L"/imgdesc/Top", 0);
                delay = MetaUInt(q.Get(), L"/grctlext/Delay", 10);
                disposal = MetaUInt(q.Get(), L"/grctlext/Disposal", 0);
            }
        }
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f,
                                    WICBitmapPaletteTypeCustom)))
            break;
        patch.resize((size_t)fw * fh);
        if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)(patch.size() * 4), (BYTE*)patch.data()))) break;

        // Undo the previous frame as it asked: 2 = clear its patch, 3 = restore what was under it.
        if (prevDisposal == 2) {
            for (LONG y = prev.top; y < prev.bottom; ++y)
                std::fill(canvas.begin() + (size_t)y * W + prev.left, canvas.begin() + (size_t)y * W + prev.right, 0u);
        } else if (prevDisposal == 3 && saved.size() == canvas.size()) {
            canvas = saved;
        }
        if (disposal == 3) saved = canvas;
        for (UINT y = 0; y < fh && top + y < H; ++y) {
            uint32_t* dst = canvas.data() + (size_t)(top + y) * W;
            const uint32_t* src = patch.data() + (size_t)y * fw;
            for (UINT x = 0; x < fw && left + x < W; ++x) {
                uint32_t s = src[x], a = s >> 24;
                if (a == 255) dst[left + x] = s;
                else if (a) dst[left + x] = Over(s, dst[left + x]);
            }
        }
        prevDisposal = disposal;
        prev = { (LONG)std::min(left, W), (LONG)std::min(top, H), (LONG)std::min(left + fw, W), (LONG)std::min(top + fh, H) };

        PetSprite::Frame frame;
        frame.px.resize((size_t)tw * th);
        ComPtr<IWICBitmap> src;
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppPBGRA, W * 4, (UINT)(canvasPx * 4),
                                               (BYTE*)canvas.data(), &src)) ||
            FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(src.Get(), (UINT)tw, (UINT)th, mode)) ||
            FAILED(sc->CopyPixels(nullptr, (UINT)tw * 4, (UINT)(frame.px.size() * 4), (BYTE*)frame.px.data())))
            break;
        if (mode == WICBitmapInterpolationModeHighQualityCubic) ClampPremultiplied(frame.px);
        float d = delay * 0.01f;
        frame.seconds = d < 0.02f ? 0.1f : d;  // like browsers: "as fast as possible" means 100 ms
        out.frames.push_back(std::move(frame));
    }
    if (out.frames.empty()) { error = L"None of the image's frames could be decoded."; return false; }
    out.w = tw;
    out.h = th;
    return true;
}

// --- The built-in blob -------------------------------------------------------------------------

namespace {
struct BlobPose { float sx, sy, lift, seconds; bool moves; bool blink; };

// One hop: settle, crouch, spring up (stretched), hang, drop, squash on landing, wobble back.
const BlobPose kHop[] = {
    { 1.00f, 1.00f, 0.00f, 0.30f, false, false }, { 1.03f, 0.97f, 0.00f, 0.07f, false, false },
    { 1.08f, 0.92f, 0.00f, 0.06f, false, false }, { 1.14f, 0.85f, 0.00f, 0.07f, false, false },
    { 0.90f, 1.13f, 0.32f, 0.05f, true, false },  { 0.93f, 1.08f, 0.64f, 0.05f, true, false },
    { 0.97f, 1.03f, 0.87f, 0.05f, true, false },  { 1.00f, 1.00f, 1.00f, 0.06f, true, false },
    { 1.00f, 1.00f, 0.97f, 0.06f, true, false },  { 0.96f, 1.04f, 0.82f, 0.05f, true, false },
    { 0.92f, 1.09f, 0.50f, 0.05f, true, false },  { 1.17f, 0.83f, 0.00f, 0.07f, false, false },
    { 1.06f, 0.94f, 0.00f, 0.06f, false, false }, { 0.97f, 1.03f, 0.00f, 0.06f, false, false },
    { 1.01f, 0.99f, 0.00f, 0.08f, false, false },
};

Color Mix(const Color& a, const Color& b, float t) { return Color::Lerp(a, b, t); }

void DrawBlob(Renderer& r, float W, float H, const BlobPose& p, const Theme& t) {
    ID2D1RenderTarget* rt = r.RT();
    ID2D1Factory* f = r.Factory();
    ID2D1SolidColorBrush* br = r.Brush();
    const Color body = t.accent;
    float bw0 = H * 0.86f, bh0 = H * 0.70f, hopH = H * 0.22f;
    float bw = bw0 * p.sx, bh = bh0 * p.sy;
    float cx = W / 2, ground = H - 0.5f;
    float yb = ground - p.lift * hopH, yt = yb - bh;
    float x0 = cx - bw / 2, x1 = cx + bw / 2;

    // Soft contact shadow that shrinks as it leaves the ground.
    r.FillEllipse({ cx, ground - H * 0.02f }, bw0 * 0.40f * (1.f - p.lift * 0.35f), H * 0.05f,
                  Color{ 0, 0, 0, 0.30f * (1.f - p.lift * 0.6f) });

    // Gum-drop body: flat, rounded bottom and a dome on top.
    float rb = bw * 0.2f, side = yt + bh * 0.6f;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return;
    sink->BeginFigure({ x0 + rb, yb }, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine({ x1 - rb, yb });
    sink->AddArc(D2D1::ArcSegment({ x1, yb - rb }, { rb, rb }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine({ x1, side });
    sink->AddArc(D2D1::ArcSegment({ cx, yt }, { bw / 2, side - yt }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment({ x0, side }, { bw / 2, side - yt }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine({ x0, yb - rb });
    sink->AddArc(D2D1::ArcSegment({ x0 + rb, yb }, { rb, rb }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    D2D1_GRADIENT_STOP stops[2] = { { 0.f, Mix(body, Color{ 1, 1, 1, 1 }, 0.22f).d2d() },
                                    { 1.f, Mix(body, Color{ 0, 0, 0, 1 }, 0.18f).d2d() } };
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> grad;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &coll)) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, yt }, { 0, yb }), coll.Get(), &grad)))
        rt->FillGeometry(geo.Get(), grad.Get());
    br->SetColor(Mix(body, t.background, 0.55f).WithAlpha(0.9f).d2d());
    rt->DrawGeometry(geo.Get(), br, std::max(1.f, H / 26.f));

    // Shine.
    r.FillEllipse({ cx - bw * 0.22f, yt + bh * 0.24f }, bw * 0.11f, bh * 0.08f, Color{ 1, 1, 1, 0.55f });

    // Face: eyes (or a blink), rosy cheeks and a tiny smile.
    float ey = yt + bh * 0.52f, ex = bw * 0.17f, erx = bw0 * 0.062f, ery = bh0 * 0.11f;
    Color dark = t.background;
    for (int s = -1; s <= 1; s += 2) {
        float x = cx + s * ex;
        if (p.blink) {
            r.FillRound({ x - erx * 1.2f, ey - 0.6f, x + erx * 1.2f, ey + 0.6f }, 0.6f, dark);
        } else {
            r.FillEllipse({ x, ey }, erx, ery, dark);
            r.FillEllipse({ x + erx * 0.35f, ey - ery * 0.4f }, erx * 0.42f, erx * 0.42f, Color{ 1, 1, 1, 0.95f });
        }
        r.FillEllipse({ cx + s * bw * 0.31f, ey + ery * 1.15f }, bw * 0.07f, bh * 0.045f, t.hot.WithAlpha(0.45f));
    }
    ComPtr<ID2D1PathGeometry> smile;
    ComPtr<ID2D1GeometrySink> ss;
    if (SUCCEEDED(f->CreatePathGeometry(&smile)) && SUCCEEDED(smile->Open(&ss))) {
        float my = ey + ery * 0.95f, mw = bw * 0.06f;
        ss->BeginFigure({ cx - mw, my }, D2D1_FIGURE_BEGIN_HOLLOW);
        ss->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ cx, my + mw * 1.1f }, { cx + mw, my }));
        ss->EndFigure(D2D1_FIGURE_END_OPEN);
        ss->Close();
        br->SetColor(dark.d2d());
        rt->DrawGeometry(smile.Get(), br, std::max(0.9f, H / 30.f));
    }
}
}  // namespace

void MakeBlobSprite(Renderer& r, int heightPx, const Theme& t, PetSprite& out) {
    out = PetSprite{};
    int H = std::max(8, heightPx), W = (int)std::lround(H * 1.3f);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, dib);
    // Two hops, blinking once in the second, so it doesn't look like a loop.
    std::vector<BlobPose> poses(std::begin(kHop), std::end(kHop));
    for (const auto& p : kHop) poses.push_back(p);
    poses[std::size(kHop)].seconds = 0.12f;
    BlobPose blink = poses[std::size(kHop)];
    blink.blink = true;
    blink.seconds = 0.09f;
    BlobPose open = poses[std::size(kHop)];
    open.seconds = 0.25f;
    poses.insert(poses.begin() + std::size(kHop) + 1, { blink, open });
    for (const auto& p : poses) {
        if (!r.Begin(dc, W, H, 96.f, true)) break;
        r.Clear(Color{ 0, 0, 0, 0 });
        DrawBlob(r, (float)W, (float)H, p, t);
        r.End();
        GdiFlush();
        PetSprite::Frame fr;
        fr.px.assign((const uint32_t*)bits, (const uint32_t*)bits + (size_t)W * H);
        fr.seconds = p.seconds;
        fr.moves = p.moves;
        out.frames.push_back(std::move(fr));
    }
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    out.w = W;
    out.h = H;
    float total = 0, moving = 0;
    for (const auto& p : poses) {
        total += p.seconds;
        if (p.moves) moving += p.seconds;
    }
    out.travel = moving > 0 ? total / moving : 1.f;
}

// --- Motion ------------------------------------------------------------------------------------

float Pet::Random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ & 0xFFFFFF) / (float)0x1000000;
}

bool Pet::Step(double now, bool haveSpace, float left, float right, float scale, int barPxH) {
    bool wasShown = shown;
    int ox = x, oy = y;
    size_t of = frame;
    bool om = mirrored;
    if (!sprite || sprite->frames.empty()) { shown = false; return wasShown; }
    const size_t n = sprite->frames.size();
    float w = sprite->w / scale;
    if (!haveSpace || right - left < w + 2.f) {
        shown = false;
        last_ = 0;
        return wasShown;
    }
    double dt = last_ > 0 ? std::clamp(now - last_, 0.0, 0.1) : 0.0;
    last_ = now;

    // The GIF's own timing.
    frame %= n;
    if (frameDue_ <= 0 || now - frameDue_ > 2.0) frameDue_ = now + sprite->frames[frame].seconds;
    for (int guard = 0; now >= frameDue_ && guard < 256; ++guard) {
        frame = (frame + 1) % n;
        frameDue_ += sprite->frames[frame].seconds;
    }

    float maxPos = right - w;
    if (!walk) {
        pos_ = left + (maxPos - left) / 2;
        dir_ = 1;
    } else {
        // First time, or the free space moved somewhere else: start at a random spot.
        if (pos_ < 0 || pos_ < left - 40.f || pos_ > maxPos + 40.f) pos_ = left + (maxPos - left) * Random();
        pos_ = std::clamp(pos_, left, maxPos);
        if (sprite->frames[frame].moves) pos_ += dir_ * speed * sprite->travel * (float)dt;
        if (pos_ <= left) { pos_ = left; dir_ = 1; }
        else if (pos_ >= maxPos) { pos_ = maxPos; dir_ = -1; }
        // Now and then it changes its mind and turns around.
        if (nextTurn_ == 0) nextTurn_ = now + 8 + Random() * 14;
        if (now >= nextTurn_) { dir_ = -dir_; nextTurn_ = now + 8 + Random() * 14; }
    }

    int ground = barPxH - std::max(1, (int)std::lround(scale));  // stands on the bar's bottom line
    int top = ground - sprite->h;
    float hop = 0;
    if (hopStart_ >= 0) {
        float u = (float)((now - hopStart_) / 0.42);
        if (u >= 1.f) hopStart_ = -1;
        else hop = std::sin(3.14159265f * u);
    }
    x = (int)std::lround(pos_ * scale);
    y = top - (int)std::lround(hop * std::min(5.f * scale, (float)std::max(top, 0)));
    mirrored = (dir_ < 0) != facesLeft;
    shown = true;
    return !wasShown || x != ox || y != oy || frame != of || mirrored != om;
}
