#include "render.h"
#include "config.h"

static bool FamilyExists(IDWriteFactory* dw, const std::wstring& name) {
    ComPtr<IDWriteFontCollection> fc;
    if (FAILED(dw->GetSystemFontCollection(&fc, FALSE))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    fc->FindFamilyName(name.c_str(), &idx, &exists);
    return exists != FALSE;
}

bool Renderer::Init(const Config& cfg) {
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts,
                                 (void**)factory_.GetAddressOf())))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dwrite_.GetAddressOf())))
        return false;
    if (SUCCEEDED(dwrite_->CreateTypography(&tabular_))) {
        DWRITE_FONT_FEATURE tnum{ DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 };
        tabular_->AddFontFeature(tnum);
    }
    if (!CreateTarget()) return false;
    SetFonts(cfg);
    return true;
}

bool Renderer::CreateTarget() {
    rt_.Reset();
    brush_.Reset();
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(factory_->CreateDCRenderTarget(&p, &rt_))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
    ++generation_;
    return true;
}

void Renderer::Shutdown() {
    for (auto& f : formats_) f.Reset();
    for (auto& e : ellipsis_) e.Reset();
    brush_.Reset();
    rt_.Reset();
    tabular_.Reset();
    dwrite_.Reset();
    factory_.Reset();
}

void Renderer::SetFonts(const Config& cfg) {
    std::wstring text = FamilyExists(dwrite_.Get(), cfg.font) ? cfg.font : L"Segoe UI";
    iconFamily_ = cfg.iconFont;
    if (iconFamily_.empty() || !FamilyExists(dwrite_.Get(), iconFamily_))
        iconFamily_ = FamilyExists(dwrite_.Get(), L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-us";
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    struct Spec { const std::wstring* family; DWRITE_FONT_WEIGHT weight; float size; };
    float s = cfg.fontSize;
    Spec specs[7] = {
        { &text, DWRITE_FONT_WEIGHT_SEMI_BOLD, s },            // Text
        { &text, DWRITE_FONT_WEIGHT_BOLD, s },                 // Bold
        { &iconFamily_, DWRITE_FONT_WEIGHT_NORMAL, s + 1.f },  // Icon
        { &text, DWRITE_FONT_WEIGHT_NORMAL, s - 1.f },         // Sub
        { &text, DWRITE_FONT_WEIGHT_NORMAL, s - 0.5f },        // Tip
        { &text, DWRITE_FONT_WEIGHT_SEMI_BOLD, s },            // TipBold
        { &iconFamily_, DWRITE_FONT_WEIGHT_NORMAL, s + 3.f },  // IconBig
    };
    for (int i = 0; i < 7; ++i) {
        formats_[i].Reset();
        ellipsis_[i].Reset();
        dwrite_->CreateTextFormat(specs[i].family->c_str(), nullptr, specs[i].weight, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, specs[i].size, locale, &formats_[i]);
        if (!formats_[i]) continue;
        formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        formats_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dwrite_->CreateEllipsisTrimmingSign(formats_[i].Get(), &ellipsis_[i]);
    }
}

IDWriteTextFormat* Renderer::Format(Font f) { return formats_[(int)f].Get(); }

bool Renderer::Begin(HDC dc, int width, int height, float dpi) {
    if (!rt_ && !CreateTarget()) return false;
    RECT rc{ 0, 0, width, height };
    if (FAILED(rt_->BindDC(dc, &rc))) return false;
    rt_->SetDpi(dpi, dpi);
    rt_->BeginDraw();
    drawing_ = true;
    return true;
}

void Renderer::End() {
    if (!drawing_) return;
    drawing_ = false;
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) CreateTarget();
}

void Renderer::Clear(const Color& c) { rt_->Clear(c.d2d()); }

void Renderer::FillRect(const D2D1_RECT_F& r, const Color& c) {
    brush_->SetColor(c.d2d());
    rt_->FillRectangle(r, brush_.Get());
}

void Renderer::FillRound(const D2D1_RECT_F& r, float radius, const Color& c) {
    brush_->SetColor(c.d2d());
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
}

void Renderer::StrokeRound(const D2D1_RECT_F& r, float radius, const Color& c, float width) {
    brush_->SetColor(c.d2d());
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), width);
}

void Renderer::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, const Color& c, float width) {
    brush_->SetColor(c.d2d());
    rt_->DrawLine(a, b, brush_.Get(), width);
}

ComPtr<IDWriteTextLayout> Renderer::Layout(const std::wstring& s, Font f, float maxW, float maxH) {
    ComPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* fmt = Format(f);
    if (!fmt) return layout;
    dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, std::max(maxW, 1.f), std::max(maxH, 1.f), &layout);
    if (layout && tabular_) layout->SetTypography(tabular_.Get(), DWRITE_TEXT_RANGE{ 0, (UINT32)s.size() });
    return layout;
}

float Renderer::Measure(const std::wstring& s, Font f) {
    if (s.empty()) return 0;
    auto layout = Layout(s, f, 10000.f, 100.f);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return std::ceil(m.widthIncludingTrailingWhitespace);
}

TextRun Renderer::MakeRun(const std::wstring& s, Font f, float height) {
    TextRun run;
    if (s.empty()) return run;
    run.layout = Layout(s, f, 10000.f, height);
    if (run.layout) {
        DWRITE_TEXT_METRICS m{};
        run.layout->GetMetrics(&m);
        run.width = std::ceil(m.widthIncludingTrailingWhitespace);
    }
    return run;
}

void Renderer::DrawRun(const TextRun& run, float x, float top, const Color& c) {
    if (!run.layout) return;
    brush_->SetColor(c.d2d());
    rt_->DrawTextLayout(D2D1::Point2F(x, top), run.layout.Get(), brush_.Get());
}

float Renderer::LineHeight(Font f) {
    auto layout = Layout(L"Ag", f, 1000.f, 1000.f);
    if (!layout) return 16.f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

void Renderer::Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, const Color& c, Align a, bool ellipsis) {
    if (s.empty()) return;
    auto layout = Layout(s, f, r.right - r.left, r.bottom - r.top);
    if (!layout) return;
    layout->SetTextAlignment(a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (ellipsis && ellipsis_[(int)f]) {
        DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        layout->SetTrimming(&t, ellipsis_[(int)f].Get());
    }
    brush_->SetColor(c.d2d());
    rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::Bitmap(ID2D1Bitmap* b, const D2D1_RECT_F& r, float opacity) {
    if (!b) return;
    rt_->DrawBitmap(b, r, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

ComPtr<ID2D1Bitmap> Renderer::CreateBitmap(const void* pixels, int w, int h) {
    ComPtr<ID2D1Bitmap> bmp;
    if (!rt_) return bmp;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    rt_->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), pixels, (UINT32)w * 4, props, &bmp);
    return bmp;
}
