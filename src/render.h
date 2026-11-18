#pragma once
#include "common.h"
#include <wincodec.h>

struct Config;

enum class Font {
    Text, Bold, Icon, Sub, Tip, TipBold, IconBig, IconHuge, Title, Label,
    TipSmall, TipSmallBold, TipKey, TipHero, TipUnit,  // tooltip cards
    Count
};
enum class Align { Left, Center, Right };

// A shaped piece of text, laid out once and drawn (possibly several times) without re-shaping.
struct TextRun {
    ComPtr<IDWriteTextLayout> layout;
    float width = 0;
};

// Software Direct2D renderer drawing into memory DCs. Software mode keeps the bar off the GPU
// entirely (no driver load, no interference with games) and the bar is tiny, so it is fast.
// Two targets: an opaque one (ClearType text) for the bar/tooltips and a premultiplied-alpha one
// (grayscale text) for floating, see-through surfaces like the power menu.
class Renderer {
public:
    bool Init(const Config& cfg);
    void Shutdown();
    void SetFonts(const Config& cfg);

    bool Begin(HDC dc, int width, int height, float dpi, bool alpha = false);
    void End();

    void Clear(const Color& c);
    void FillRect(const D2D1_RECT_F& r, const Color& c);
    void FillRound(const D2D1_RECT_F& r, float radius, const Color& c);
    void StrokeRound(const D2D1_RECT_F& r, float radius, const Color& c, float width);
    void FillEllipse(D2D1_POINT_2F c, float rx, float ry, const Color& col);
    void StrokeEllipse(D2D1_POINT_2F c, float rx, float ry, const Color& col, float width);
    // Arc from startDeg (0 = 12 o'clock, clockwise) sweeping sweepDeg degrees.
    void Arc(D2D1_POINT_2F c, float radius, float startDeg, float sweepDeg, const Color& col, float width);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, const Color& c, float width);
    // Line chart of values / maxValue right-aligned in r (`capacity` points span the full width),
    // filled with a vertical gradient from fillTop (at the top of r) to fillBottom.
    void AreaChart(const D2D1_RECT_F& r, const std::vector<float>& values, size_t capacity, float maxValue,
                   const Color& line, const Color& fillTop, const Color& fillBottom);
    void FillRectGradientV(const D2D1_RECT_F& r, const Color& top, const Color& bottom);
    void DottedLine(D2D1_POINT_2F a, D2D1_POINT_2F b, const Color& c, float width);
    // Distance from the top of a line of text to its baseline.
    float Baseline(Font f);
    // Text is vertically centred in r.
    void Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, const Color& c, Align a = Align::Left,
              bool ellipsis = false);
    float Measure(const std::wstring& s, Font f);
    TextRun MakeRun(const std::wstring& s, Font f, float height);
    void DrawRun(const TextRun& run, float x, float top, const Color& c);
    float LineHeight(Font f);
    void Bitmap(ID2D1Bitmap* b, const D2D1_RECT_F& r, float opacity = 1.f);
    void SetTransform(const D2D1_MATRIX_3X2_F& m);
    void ResetTransform();

    // Text in the UI font at an exact size (in the target's units), for surfaces that size their own text.
    ComPtr<IDWriteTextFormat> CreateTextFormat(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const Color& c, Align a = Align::Left);
    float Measure(const std::wstring& s, IDWriteTextFormat* f);
    float LineHeight(IDWriteTextFormat* f);

    // Premultiplied BGRA, top-down rows. Bitmaps belong to the opaque target.
    ComPtr<ID2D1Bitmap> CreateBitmap(const void* pixels, int w, int h);
    // Decodes a PNG resource once and scales it (high-quality cubic) to each requested square size.
    // These bitmaps belong to the see-through target. Returns false if the image can't be loaded.
    bool LoadPngResource(HINSTANCE inst, int id, const int* sizes, int count, ComPtr<ID2D1Bitmap>* out);

    void ReleaseAlphaTarget() { alpha_ = Target{}; wic_.Reset(); }  // free the see-through surface after rare use
    unsigned Generation() const { return generation_; }  // bumps when bitmaps must be recreated
    const std::wstring& IconFamily() const { return iconFamily_; }

    // For custom drawing (geometry) while a target is bound.
    ID2D1Factory* Factory() const { return factory_.Get(); }
    ID2D1RenderTarget* RT() const { return rt_; }
    ID2D1SolidColorBrush* Brush() const { return brush_; }

private:
    struct Target {
        ComPtr<ID2D1DCRenderTarget> rt;
        ComPtr<ID2D1SolidColorBrush> brush;
    };
    bool CreateTarget(Target& t, bool alpha);
    IDWriteTextFormat* Format(Font f);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, Font f, float maxW, float maxH);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, IDWriteTextFormat* fmt, float maxW, float maxH);

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    std::wstring textFamily_ = L"Segoe UI";
    Target opaque_, alpha_;
    ID2D1DCRenderTarget* rt_ = nullptr;          // current target while drawing
    ID2D1SolidColorBrush* brush_ = nullptr;
    bool currentAlpha_ = false;
    ComPtr<IDWriteTypography> tabular_;
    ComPtr<IDWriteInlineObject> ellipsis_[(int)Font::Count];
    ComPtr<IDWriteTextFormat> formats_[(int)Font::Count];
    float baselines_[(int)Font::Count] = {};
    float lineHeights_[(int)Font::Count] = {};
    ComPtr<ID2D1StrokeStyle> roundStroke_, dotStroke_;
    std::wstring iconFamily_;
    unsigned generation_ = 1;
    bool drawing_ = false;
};
