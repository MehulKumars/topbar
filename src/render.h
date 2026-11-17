#pragma once
#include "common.h"

struct Config;

enum class Font { Text, Bold, Icon, Sub, Tip, TipBold, IconBig };
enum class Align { Left, Center, Right };

// A shaped piece of text, laid out once and drawn (possibly several times) without re-shaping.
struct TextRun {
    ComPtr<IDWriteTextLayout> layout;
    float width = 0;
};

// Software Direct2D renderer drawing into memory DCs. Software mode keeps the bar off the GPU
// entirely (no driver load, no interference with games) and the bar is tiny, so it is fast.
class Renderer {
public:
    bool Init(const Config& cfg);
    void Shutdown();
    void SetFonts(const Config& cfg);

    bool Begin(HDC dc, int width, int height, float dpi);
    void End();

    void Clear(const Color& c);
    void FillRect(const D2D1_RECT_F& r, const Color& c);
    void FillRound(const D2D1_RECT_F& r, float radius, const Color& c);
    void StrokeRound(const D2D1_RECT_F& r, float radius, const Color& c, float width);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, const Color& c, float width);
    // Text is vertically centred in r.
    void Text(const std::wstring& s, Font f, const D2D1_RECT_F& r, const Color& c, Align a = Align::Left,
              bool ellipsis = false);
    float Measure(const std::wstring& s, Font f);
    TextRun MakeRun(const std::wstring& s, Font f, float height);
    void DrawRun(const TextRun& run, float x, float top, const Color& c);
    float LineHeight(Font f);
    void Bitmap(ID2D1Bitmap* b, const D2D1_RECT_F& r, float opacity = 1.f);

    // Premultiplied BGRA, top-down rows.
    ComPtr<ID2D1Bitmap> CreateBitmap(const void* pixels, int w, int h);

    unsigned Generation() const { return generation_; }  // bumps when GPU/D2D resources were recreated
    const std::wstring& IconFamily() const { return iconFamily_; }

private:
    bool CreateTarget();
    IDWriteTextFormat* Format(Font f);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, Font f, float maxW, float maxH);

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1DCRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTypography> tabular_;
    ComPtr<IDWriteInlineObject> ellipsis_[7];
    ComPtr<IDWriteTextFormat> formats_[7];
    std::wstring iconFamily_;
    unsigned generation_ = 1;
    bool drawing_ = false;
};
