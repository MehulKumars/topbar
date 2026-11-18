// The bar pet: a small animated sprite (any GIF, or the built-in blob) that walks along the empty
// part of the bar. Decoded once into pre-scaled frames; drawing a frame only touches its own pixels.
#pragma once
#include "common.h"

class Renderer;
struct Theme;

struct PetSprite {
    struct Frame {
        std::vector<uint32_t> px;   // premultiplied BGRA, w * h, top-down
        float seconds = 0.1f;       // how long the frame stays up
        bool moves = true;          // the pet only travels on frames that move (the blob hops)
    };
    int w = 0, h = 0;               // pixels
    std::vector<Frame> frames;
    float travel = 1.f;             // speed boost on moving frames, so the average pace is `speed`
};

// Decodes an animated GIF (or any still image Windows can read) into frames `heightPx` tall.
// Runs on a worker thread. Only local files are accepted, and files, images and frame counts
// are capped so a hostile or broken file can't eat memory or time. smooth: -1 auto, 0, 1.
bool LoadPetSprite(const std::wstring& path, int heightPx, int smooth, PetSprite& out, std::wstring& error);

// The built-in companion: a little blob in the theme's accent colour that hops along. UI thread.
void MakeBlobSprite(Renderer& r, int heightPx, const Theme& t, PetSprite& out);

class Pet {
public:
    std::shared_ptr<const PetSprite> sprite;
    bool walk = true;
    float speed = 24.f;             // DIPs per second
    bool facesLeft = false;

    void Reset() { pos_ = -1; last_ = 0; frameDue_ = 0; frame = 0; hopStart_ = -1; shown = false; }
    // Moves on to `now` inside [left, right] (DIPs). Returns true when the picture changed.
    bool Step(double now, bool haveSpace, float left, float right, float scale, int barPxH);
    void Hop(double now) { if (hopStart_ < 0) hopStart_ = now; }
    bool Hopping() const { return hopStart_ >= 0; }
    float CenterDip(float scale) const { return sprite && pos_ >= 0 ? pos_ + sprite->w / scale / 2 : -1.f; }
    double NextFrameIn(double now) const { return frameDue_ - now; }

    // What to draw, in bar pixels.
    bool shown = false;
    int x = 0, y = 0;
    size_t frame = 0;
    bool mirrored = false;

private:
    float pos_ = -1;                // left edge, DIPs
    int dir_ = 1;
    double last_ = 0, frameDue_ = 0, hopStart_ = -1, nextTurn_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    float Random();
};
