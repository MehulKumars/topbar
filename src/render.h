#pragma once
#include "common.h"

struct Config;

enum class Font { Text, Bold, Icon, Sub, Tip, TipBold, IconBig, IconHuge, Title, Label, Count };
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
    // Filled line chart of values (0..1) right-aligned in r; `capacity` points span the full width.
    void AreaChart(const D2D1_RECT_F& r, const std::vector<float>& values, size_t capacity, const Color& line,
                   const Color& fill);
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

    // Premultiplied BGRA, top-down rows. Bitmaps belong to the opaque target.
    ComPtr<ID2D1Bitmap> CreateBitmap(const void* pixels, int w, int h);

    unsigned Generation() const { return generation_; }  // bumps when bitmaps must be recreated
    const std::wstring& IconFamily() const { return iconFamily_; }

private:
    struct Target {
        ComPtr<ID2D1DCRenderTarget> rt;
        ComPtr<ID2D1SolidColorBrush> brush;
    };
    bool CreateTarget(Target& t, bool alpha);
    IDWriteTextFormat* Format(Font f);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, Font f, float maxW, float maxH);

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    Target opaque_, alpha_;
    ID2D1DCRenderTarget* rt_ = nullptr;          // current target while drawing
    ID2D1SolidColorBrush* brush_ = nullptr;
    bool currentAlpha_ = false;
    ComPtr<IDWriteTypography> tabular_;
    ComPtr<IDWriteInlineObject> ellipsis_[(int)Font::Count];
    ComPtr<IDWriteTextFormat> formats_[(int)Font::Count];
    std::wstring iconFamily_;
    unsigned generation_ = 1;
    bool drawing_ = false;
};
