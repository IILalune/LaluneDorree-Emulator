// ppu.hpp - Picture Processing Unit (graphics).
//
// The screen is drawn line by line, like a CRT: 160 visible lines followed by
// 68 lines of "vertical blank" when games safely update video memory. Each
// line takes 1232 CPU cycles: 960 drawing pixels, 272 in "horizontal blank".
//
// We render a whole scanline at the start of its HBlank, composing up to four
// background layers and the sprite layer according to their priorities.
#pragma once

#include <array>

#include "common.hpp"

class GBA;
class StateStream;

class PPU {
public:
    explicit PPU(GBA& gba);
    void reset();
    void tick(int cycles);
    int cyclesUntilNextEvent() const;

    u16 vcount() const { return vcount_; }
    void latchAffine(int index);  // BG2X/BG2Y etc. were written
    // Everything except `framebuffer`: the next frame redraws it, and save
    // state files keep it as their screenshot (see save_states.hpp).
    void serialize(StateStream& s);

    bool frameReady = false;
    // Final image, one 0xAARRGGBB value per pixel.
    std::array<u32, kScreenWidth * kScreenHeight> framebuffer{};

private:
    static constexpr u16 kTransparent = 0x8000;

    struct ObjPixel {
        u16 color = kTransparent;
        u8 priority = 4;
        bool semiTransparent = false;
    };

    GBA& gba_;
    int lineCycles_ = 0;
    bool inHBlank_ = false;
    u16 vcount_ = 0;

    // Internal affine reference points for BG2/BG3 (advance every line).
    s32 affineX_[2]{}, affineY_[2]{};

    std::array<std::array<u16, kScreenWidth>, 4> bgLine_{};
    std::array<ObjPixel, kScreenWidth> objLine_{};
    std::array<bool, kScreenWidth> objWindow_{};

    void startHBlank();
    void startLine();
    void checkVCount();
    void reloadAffine();

    void renderLine();
    void renderTextBg(int bg);
    void renderAffineBg(int bg);
    void renderBitmap(int mode);
    void renderSprites();
    void composite();

    u16 io(u32 offset) const;
    u16 palette(u32 index) const;
    static u32 toRgb(u16 color);
};
