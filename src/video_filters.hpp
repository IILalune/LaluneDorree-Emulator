// video_filters.hpp - Turns the 240x160 GBA frame into a nicer, bigger image.
//
// All filters run on the CPU once per displayed frame (a GBA frame is only
// 38,400 pixels, so this takes a few milliseconds). The result is uploaded to
// the GPU, which does the final smooth scaling to the window size.
//
//   Pixel perfect  Sharp square pixels, exactly like the original.
//   Smooth HD      xBR 4x: finds edges in the pixel art and redraws them as
//                  smooth lines and curves, so sprites look like hi-res art.
//   Cinematic HD   Smooth HD plus richer colour, a soft glow around bright
//                  areas and a gentle vignette.
//   Retro LCD      Big pixels with the dark grid lines of a handheld screen.
//   Super HD       Super-xBR 8x: a true HD image (1920x1280). Every pixel is
//                  interpolated along the direction of the edges around it,
//                  so the art looks repainted at high resolution rather than
//                  scaled. Spread over all CPU cores.
//   Retro CRT      A 90s TV: curved glass with rounded corners, glowing
//                  scanlines, RGB phosphor stripes, colour fringes at the
//                  edges and bloom around bright areas.
//   LCD Bloom      Retro LCD where every pixel glows: light spills over the
//                  grid lines and bright areas bloom.
//
// Retro CRT and LCD Bloom mix light in linear space (like a real screen
// adds light) and roll off highlights softly instead of clipping them.
#pragma once

#include <vector>

#include "common.hpp"

// New filters go at the end: settings files store the number.
enum class VideoFilter { PixelPerfect, SmoothHD, CinematicHD, RetroLCD, SuperHD, RetroCRT, LCDBloom };
constexpr int kVideoFilterCount = 7;
const char* videoFilterName(VideoFilter filter);

class VideoProcessor {
public:
    VideoProcessor();

    // Processes one frame. `prescale` is used by Pixel perfect: it enlarges
    // each pixel to a prescale x prescale block so the GPU's smooth scaling
    // only blurs the block edges (sharp, even pixels at any window size).
    // Retro CRT also renders at about this scale (4x to 6x), so its
    // scanlines and phosphor stripes land on whole screen pixels.
    void process(VideoFilter filter, const u32* frame, int prescale);

    // The processed image (0xAARRGGBB pixels).
    const std::vector<u32>& image() const { return image_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Cinematic HD only: a blurred 240x160 image of the bright parts of the
    // frame, meant to be drawn on top with additive blending.
    const std::vector<u32>& glow() const { return glow_; }
    bool hasGlow() const { return hasGlow_; }

private:
    static constexpr int kScale = 4;  // xBR and Retro LCD output size
    static constexpr int kPad = 2;    // the xBR window reaches 2 pixels out

    struct Yuv {
        int y, u, v;
    };

    std::vector<u32> image_, glow_, graded_;
    int width_ = 0, height_ = 0;
    bool hasGlow_ = false;

    // Padded copies of the frame (edges repeated) so xBR never reads out of bounds.
    std::vector<u32> paddedColor_;
    std::vector<Yuv> paddedYuv_;

    // Super-xBR works in 2x steps; these hold the image (and its brightness)
    // between steps.
    std::vector<u32> stepColor_[2];
    std::vector<float> stepLuma_[2];

    // Precomputed per corner (4 rotations) and per output sub-pixel (4x4):
    // how much of that sub-pixel each xBR edge shape covers (0-256).
    int coverMain_[4][16], coverShallow_[4][16], coverSteep_[4][16];
    int neighbor_[4][12];  // index offsets of the neighbours, rotated per corner

    // Linear light (Retro CRT, LCD Bloom).
    static constexpr int kEncodeSize = 4096;
    static constexpr float kEncodeMax = 4.0f;  // brightest linear value the encoder accepts
    static constexpr int kBeamSteps = 64;
    float toLinear_[256];    // 8-bit sRGB -> linear 0..1
    u8 encode_[kEncodeSize]; // linear (indexed by its square root) -> 8-bit sRGB, highlights rolled off
    float beam_[kBeamSteps + 1][kBeamSteps + 1];  // CRT scanline brightness by [colour][distance]
    std::vector<float> linear_, bloom_, halo_, blurTmp_;  // 240x160, RGB interleaved

    void nearest(const u32* frame, int scale);
    void xbr(const u32* frame);
    void retroLcd(const u32* frame);
    void retroCrt(const u32* frame, int scale);
    void lcdBloom(const u32* frame);
    void makeLinear(const u32* frame);
    void makeBloom(float threshold, int radius, int passes, std::vector<float>& out);
    int encode(float v) const;
    void superXbr(const u32* frame);
    static void superXbrDouble(const std::vector<u32>& src, const std::vector<float>& srcLuma, int w, int h,
                               std::vector<u32>& dst, std::vector<float>& dstLuma);
    void grade(const u32* frame);
    void makeGlow(const u32* frame);
};
