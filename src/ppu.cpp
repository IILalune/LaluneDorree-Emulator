#include "ppu.hpp"

#include <algorithm>

#include "gba.hpp"
#include "state.hpp"

namespace {
constexpr int kDrawCycles = 960;    // cycles spent drawing a visible line
constexpr int kLineCycles = 1232;   // total cycles per line, including HBlank
constexpr int kTotalLines = 228;

// Sprite sizes indexed by [shape][size] -> {width, height}.
constexpr int kObjSize[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},  // square
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},  // wide
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},  // tall
};

// Sign-extend the 28-bit fixed-point affine reference point registers.
s32 signExtend28(u32 value) { return static_cast<s32>(value << 4) >> 4; }
}  // namespace

PPU::PPU(GBA& gba) : gba_(gba) {}

void PPU::reset() {
    lineCycles_ = 0;
    inHBlank_ = false;
    vcount_ = 0;
    frameReady = false;
    framebuffer.fill(0xFF000000);
    for (int i = 0; i < 2; ++i) affineX_[i] = affineY_[i] = 0;
}

u16 PPU::io(u32 offset) const { return gba_.bus.io[offset >> 1]; }

u16 PPU::palette(u32 index) const { return loadLE<u16>(gba_.bus.pram.data(), (index & 0x1FF) * 2) & 0x7FFF; }

u32 PPU::toRgb(u16 color) {
    // GBA colors are 15-bit BGR (5 bits each). Expand 5 -> 8 bits by
    // repeating the top bits so 31 maps to 255 exactly.
    const u32 r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}

int PPU::cyclesUntilNextEvent() const {
    return (inHBlank_ ? kLineCycles : kDrawCycles) - lineCycles_;
}

void PPU::tick(int cycles) {
    lineCycles_ += cycles;
    for (;;) {
        if (!inHBlank_) {
            if (lineCycles_ < kDrawCycles) break;
            startHBlank();
        } else {
            if (lineCycles_ < kLineCycles) break;
            lineCycles_ -= kLineCycles;
            startLine();
        }
    }
}

void PPU::startHBlank() {
    inHBlank_ = true;
    u16& dispstat = gba_.bus.reg(0x004);
    dispstat |= 0x2;
    if (vcount_ < kScreenHeight) {
        renderLine();
        // The affine backgrounds step down one line: add dmx (PB) and dmy (PD).
        for (int i = 0; i < 2; ++i) {
            affineX_[i] += static_cast<s16>(io(0x22 + i * 0x10));
            affineY_[i] += static_cast<s16>(io(0x26 + i * 0x10));
        }
        gba_.dma.onHBlank();
    }
    if (dispstat & 0x10) gba_.raiseIrq(IrqHBlank);
}

void PPU::startLine() {
    inHBlank_ = false;
    u16& dispstat = gba_.bus.reg(0x004);
    dispstat &= ~0x2;
    vcount_ = static_cast<u16>((vcount_ + 1) % kTotalLines);

    if (vcount_ == kScreenHeight) {  // entering vertical blank
        dispstat |= 0x1;
        if (dispstat & 0x08) gba_.raiseIrq(IrqVBlank);
        gba_.dma.onVBlank();
        reloadAffine();
        frameReady = true;
    } else if (vcount_ == kTotalLines - 1) {
        dispstat &= ~0x1;
    }
    checkVCount();
}

void PPU::checkVCount() {
    u16& dispstat = gba_.bus.reg(0x004);
    if (vcount_ == (dispstat >> 8)) {
        dispstat |= 0x4;
        if (dispstat & 0x20) gba_.raiseIrq(IrqVCount);
    } else {
        dispstat &= ~0x4;
    }
}

void PPU::latchAffine(int i) {
    const u32 base = 0x28 + i * 0x10;
    affineX_[i] = signExtend28(io(base) | (static_cast<u32>(io(base + 2)) << 16));
    affineY_[i] = signExtend28(io(base + 4) | (static_cast<u32>(io(base + 6)) << 16));
}

void PPU::reloadAffine() {
    latchAffine(0);
    latchAffine(1);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void PPU::renderLine() {
    const u16 dispcnt = io(0x000);
    u32* out = &framebuffer[vcount_ * kScreenWidth];

    if (dispcnt & 0x80) {  // forced blank: the screen shows white
        std::fill(out, out + kScreenWidth, 0xFFFFFFFFu);
        return;
    }

    for (auto& line : bgLine_) line.fill(kTransparent);
    objLine_.fill(ObjPixel{});
    objWindow_.fill(false);

    const int mode = dispcnt & 7;
    auto enabled = [dispcnt](int bg) { return dispcnt & (0x100 << bg); };
    switch (mode) {
    case 0:
        for (int bg = 0; bg < 4; ++bg) if (enabled(bg)) renderTextBg(bg);
        break;
    case 1:
        for (int bg = 0; bg < 2; ++bg) if (enabled(bg)) renderTextBg(bg);
        if (enabled(2)) renderAffineBg(2);
        break;
    case 2:
        for (int bg = 2; bg < 4; ++bg) if (enabled(bg)) renderAffineBg(bg);
        break;
    case 3: case 4: case 5:
        if (enabled(2)) renderBitmap(mode);
        break;
    default: break;
    }
    if (dispcnt & 0x1000) renderSprites();
    composite();
}

void PPU::renderTextBg(int bg) {
    const auto& vram = gba_.bus.vram;
    const u16 cnt = io(0x08 + bg * 2);
    const u32 charBase = ((cnt >> 2) & 3) * 0x4000;
    const u32 screenBase = ((cnt >> 8) & 31) * 0x800;
    const bool colors256 = cnt & 0x80;
    const int width = (cnt & 0x4000) ? 512 : 256;
    const int height = (cnt & 0x8000) ? 512 : 256;
    const int hofs = io(0x10 + bg * 4) & 0x1FF;
    const int vofs = io(0x12 + bg * 4) & 0x1FF;

    // Mosaic makes pixels into big blocks by reusing the top-left sample.
    const bool mosaic = cnt & 0x40;
    const int mosaicW = mosaic ? (io(0x4C) & 15) + 1 : 1;
    const int mosaicH = mosaic ? ((io(0x4C) >> 4) & 15) + 1 : 1;

    const int line = vcount_ - (vcount_ % mosaicH);
    const int y = (line + vofs) & (height - 1);
    auto& dest = bgLine_[bg];

    for (int x = 0; x < kScreenWidth; ++x) {
        const int sx = x - (x % mosaicW);
        const int mx = (sx + hofs) & (width - 1);
        // Big maps are made of 256x256 "screen blocks" of 32x32 tiles each.
        const u32 block = (mx >> 8) + (y >> 8) * (width == 512 ? 2 : 1);
        const u32 mapAddr = screenBase + block * 0x800 + (((y & 255) >> 3) * 32 + ((mx & 255) >> 3)) * 2;
        if (mapAddr >= 0x10000) continue;
        const u16 entry = loadLE<u16>(vram.data(), mapAddr);

        const u32 tile = entry & 0x3FF;
        int px = mx & 7, py = y & 7;
        if (entry & 0x400) px = 7 - px;  // horizontal flip
        if (entry & 0x800) py = 7 - py;  // vertical flip

        u32 colorIndex;
        if (colors256) {
            const u32 addr = charBase + tile * 64 + py * 8 + px;
            if (addr >= 0x10000) continue;
            colorIndex = vram[addr];
            if (colorIndex) dest[x] = palette(colorIndex);
        } else {
            const u32 addr = charBase + tile * 32 + py * 4 + px / 2;
            if (addr >= 0x10000) continue;
            colorIndex = (px & 1) ? (vram[addr] >> 4) : (vram[addr] & 15);
            if (colorIndex) dest[x] = palette((entry >> 12) * 16 + colorIndex);
        }
    }
}

void PPU::renderAffineBg(int bg) {
    const auto& vram = gba_.bus.vram;
    const int i = bg - 2;
    const u16 cnt = io(0x08 + bg * 2);
    const u32 charBase = ((cnt >> 2) & 3) * 0x4000;
    const u32 screenBase = ((cnt >> 8) & 31) * 0x800;
    const int size = 128 << (cnt >> 14);  // 128, 256, 512 or 1024 pixels
    const bool wrap = cnt & 0x2000;
    const s32 pa = static_cast<s16>(io(0x20 + i * 0x10));
    const s32 pc = static_cast<s16>(io(0x24 + i * 0x10));
    auto& dest = bgLine_[bg];

    // Each screen pixel maps to texture coordinates through a 2x2 matrix
    // (in 8.8 fixed point). This is what lets the GBA rotate and scale.
    s32 tx = affineX_[i], ty = affineY_[i];
    for (int x = 0; x < kScreenWidth; ++x, tx += pa, ty += pc) {
        int px = tx >> 8, py = ty >> 8;
        if (wrap) { px &= size - 1; py &= size - 1; }
        else if (px < 0 || py < 0 || px >= size || py >= size) continue;

        const u32 tile = vram[(screenBase + (py >> 3) * (size >> 3) + (px >> 3)) & 0xFFFF];
        const u32 addr = charBase + tile * 64 + (py & 7) * 8 + (px & 7);
        if (addr >= 0x10000) continue;
        const u32 colorIndex = vram[addr];
        if (colorIndex) dest[x] = palette(colorIndex);
    }
}

void PPU::renderBitmap(int mode) {
    const auto& vram = gba_.bus.vram;
    const u16 dispcnt = io(0x000);
    const u32 frame = (mode != 3 && (dispcnt & 0x10)) ? 0xA000 : 0;
    const int width = mode == 5 ? 160 : 240;
    const int height = mode == 5 ? 128 : 160;
    const s32 pa = static_cast<s16>(io(0x20));
    const s32 pc = static_cast<s16>(io(0x24));
    auto& dest = bgLine_[2];

    s32 tx = affineX_[0], ty = affineY_[0];
    for (int x = 0; x < kScreenWidth; ++x, tx += pa, ty += pc) {
        const int px = tx >> 8, py = ty >> 8;
        if (px < 0 || py < 0 || px >= width || py >= height) continue;
        if (mode == 4) {
            const u32 colorIndex = vram[frame + py * width + px];
            if (colorIndex) dest[x] = palette(colorIndex);
        } else {
            dest[x] = loadLE<u16>(vram.data(), frame + (py * width + px) * 2) & 0x7FFF;
        }
    }
}

void PPU::renderSprites() {
    const auto& vram = gba_.bus.vram;
    const auto& oam = gba_.bus.oam;
    const u16 dispcnt = io(0x000);
    const bool mapping1d = dispcnt & 0x40;
    const bool bitmapMode = (dispcnt & 7) >= 3;
    const bool objWindowEnabled = dispcnt & 0x8000;
    const int line = vcount_;

    // Lower OAM indices are drawn on top, so walk forward and only replace a
    // pixel if the later sprite has strictly better (lower) priority.
    for (int i = 0; i < 128; ++i) {
        const u16 attr0 = loadLE<u16>(oam.data(), i * 8);
        const u16 attr1 = loadLE<u16>(oam.data(), i * 8 + 2);
        const u16 attr2 = loadLE<u16>(oam.data(), i * 8 + 4);

        const bool affine = attr0 & 0x100;
        if (!affine && (attr0 & 0x200)) continue;  // sprite hidden
        const int objMode = (attr0 >> 10) & 3;
        if (objMode == 3) continue;
        const int shape = attr0 >> 14;
        if (shape == 3) continue;

        const int w = kObjSize[shape][attr1 >> 14][0];
        const int h = kObjSize[shape][attr1 >> 14][1];
        const bool doubleSize = affine && (attr0 & 0x200);
        const int boundsW = doubleSize ? w * 2 : w;
        const int boundsH = doubleSize ? h * 2 : h;

        const int dy = (line - (attr0 & 0xFF)) & 0xFF;  // Y wraps around at 256
        if (dy >= boundsH) continue;
        int x0 = attr1 & 0x1FF;
        if (x0 >= kScreenWidth) x0 -= 512;              // X is a 9-bit signed value

        const bool colors256 = attr0 & 0x2000;
        const u32 tileBase = attr2 & 0x3FF;
        if (bitmapMode && tileBase < 512) continue;     // that VRAM holds the bitmap
        const u8 priority = (attr2 >> 10) & 3;
        const u32 paletteBank = attr2 >> 12;

        s32 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const u32 group = ((attr1 >> 9) & 31) * 32;
            pa = static_cast<s16>(loadLE<u16>(oam.data(), group + 6));
            pb = static_cast<s16>(loadLE<u16>(oam.data(), group + 14));
            pc = static_cast<s16>(loadLE<u16>(oam.data(), group + 22));
            pd = static_cast<s16>(loadLE<u16>(oam.data(), group + 30));
        }

        for (int sx = 0; sx < boundsW; ++sx) {
            const int screenX = x0 + sx;
            if (screenX < 0 || screenX >= kScreenWidth) continue;

            int tx, ty;
            if (affine) {
                const int cx = sx - boundsW / 2, cy = dy - boundsH / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (attr1 & 0x1000) ? w - 1 - sx : sx;  // horizontal flip
                ty = (attr1 & 0x2000) ? h - 1 - dy : dy;  // vertical flip
            }

            // Find which 8x8 tile of the sprite this pixel is in. In "1D"
            // mapping the sprite's tiles are stored one after another; in
            // "2D" mapping VRAM is a 32-tile-wide sheet.
            const int tileX = tx >> 3, tileY = ty >> 3;
            u32 colorIndex;
            u16 color;
            if (colors256) {
                const u32 tile = mapping1d ? tileBase + tileY * (w / 8) * 2 + tileX * 2
                                           : (tileBase & ~1u) + tileY * 32 + tileX * 2;
                // Sprite VRAM is 32 KiB and wraps, so tile 1023 of a 256-colour
                // sprite can't read past the end of VRAM.
                const u32 addr = 0x10000 + (((tile & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7)) & 0x7FFF);
                colorIndex = vram[addr];
                color = palette(256 + colorIndex);
            } else {
                const u32 tile = mapping1d ? tileBase + tileY * (w / 8) + tileX : tileBase + tileY * 32 + tileX;
                const u32 addr = 0x10000 + (tile & 0x3FF) * 32 + (ty & 7) * 4 + (tx & 7) / 2;
                colorIndex = (tx & 1) ? (vram[addr] >> 4) : (vram[addr] & 15);
                color = palette(256 + paletteBank * 16 + colorIndex);
            }
            if (colorIndex == 0) continue;  // color 0 is transparent

            if (objMode == 2) {  // "OBJ window" sprites are masks, not visible
                if (objWindowEnabled) objWindow_[screenX] = true;
                continue;
            }
            ObjPixel& px = objLine_[screenX];
            if (px.color == kTransparent || priority < px.priority) {
                px.color = color;
                px.priority = priority;
                px.semiTransparent = objMode == 1;
            }
        }
    }
}

void PPU::composite() {
    const u16 dispcnt = io(0x000);
    const u16 bldcnt = io(0x50);
    const u16 bldalpha = io(0x52);
    const int eva = std::min(16, bldalpha & 31);
    const int evb = std::min(16, (bldalpha >> 8) & 31);
    const int evy = std::min(16, io(0x54) & 31);
    const int blendMode = (bldcnt >> 6) & 3;

    // Windows: rectangular regions (and a sprite-shaped one) that can hide
    // layers or disable blending.
    const bool win0 = dispcnt & 0x2000, win1 = dispcnt & 0x4000;
    const bool objWin = (dispcnt & 0x8000) && (dispcnt & 0x1000);
    const bool anyWindow = win0 || win1 || objWin;
    const u16 winIn = io(0x48), winOut = io(0x4A);

    // Window edges are [lo, hi). If lo > hi the window wraps around the edge.
    auto inRange = [](int v, int lo, int hi, int limit) {
        if (lo > hi) return v >= lo || v < hi;
        return v >= lo && v < std::min(hi, limit);
    };
    const bool win0Y = win0 && inRange(vcount_, io(0x44) >> 8, io(0x44) & 0xFF, 228);
    const bool win1Y = win1 && inRange(vcount_, io(0x46) >> 8, io(0x46) & 0xFF, 228);

    int bgPriority[4];
    for (int bg = 0; bg < 4; ++bg) bgPriority[bg] = io(0x08 + bg * 2) & 3;

    const u16 backdrop = palette(0);
    u32* out = &framebuffer[vcount_ * kScreenWidth];

    auto blendAlpha = [&](u16 a, u16 b) {
        u16 result = 0;
        for (int shift = 0; shift <= 10; shift += 5) {
            const int c = std::min(31, (((a >> shift) & 31) * eva + ((b >> shift) & 31) * evb) >> 4);
            result |= static_cast<u16>(c << shift);
        }
        return result;
    };

    for (int x = 0; x < kScreenWidth; ++x) {
        u8 mask = 0x3F;  // bits 0-3 BGs, 4 OBJ, 5 color effects
        if (anyWindow) {
            if (win0Y && inRange(x, io(0x40) >> 8, io(0x40) & 0xFF, 240)) mask = winIn & 0x3F;
            else if (win1Y && inRange(x, io(0x42) >> 8, io(0x42) & 0xFF, 240)) mask = (winIn >> 8) & 0x3F;
            else if (objWin && objWindow_[x]) mask = (winOut >> 8) & 0x3F;
            else mask = winOut & 0x3F;
        }

        // Find the top two visible layers. Layer ids: 0-3 BG, 4 OBJ, 5 backdrop.
        int layer[2] = {5, 5};
        u16 color[2] = {backdrop, backdrop};
        int found = 0;
        const ObjPixel& obj = objLine_[x];
        for (int p = 0; p < 4 && found < 2; ++p) {
            if (obj.color != kTransparent && obj.priority == p && (mask & 0x10)) {
                layer[found] = 4;
                color[found] = obj.color;
                ++found;
            }
            for (int bg = 0; bg < 4 && found < 2; ++bg) {
                if (bgPriority[bg] == p && (mask & (1 << bg)) && bgLine_[bg][x] != kTransparent) {
                    layer[found] = bg;
                    color[found] = bgLine_[bg][x];
                    ++found;
                }
            }
        }

        u16 result = color[0];
        const bool firstTarget = bldcnt & (1 << layer[0]);
        const bool secondTarget = bldcnt & (1 << (8 + layer[1]));
        if (layer[0] == 4 && obj.semiTransparent && secondTarget) {
            result = blendAlpha(color[0], color[1]);  // semi-transparent sprites always blend
        } else if ((mask & 0x20) && firstTarget) {
            if (blendMode == 1 && secondTarget) {
                result = blendAlpha(color[0], color[1]);
            } else if (blendMode == 2 || blendMode == 3) {  // fade to white / black
                result = 0;
                for (int shift = 0; shift <= 10; shift += 5) {
                    int c = (color[0] >> shift) & 31;
                    c = blendMode == 2 ? c + (((31 - c) * evy) >> 4) : c - ((c * evy) >> 4);
                    result |= static_cast<u16>(c << shift);
                }
            }
        }
        out[x] = toRgb(result);
    }
}

void PPU::serialize(StateStream& s) {
    s.section("PPU ");
    s.value(frameReady);
    s.value(lineCycles_);
    s.value(inHBlank_);
    s.value(vcount_);
    s.value(affineX_);
    s.value(affineY_);
    s.value(bgLine_);
    s.value(objLine_);
    s.value(objWindow_);
    if (s.loading() && vcount_ >= 228) vcount_ = 0;  // vcount_ picks the framebuffer line to draw
}
