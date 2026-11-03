#include "video_filters.hpp"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

constexpr int kW = kScreenWidth, kH = kScreenHeight;  // frame size

// Neighbours used by xBR, named as in the original algorithm, written for
// the bottom-right corner of the centre pixel E:
//
//          B  C
//       D  E  F  F4
//       G  H  I  I4
//          H5 I5
enum Neighbor { B, C, D, F, G, H, I, F4, I4, H5, I5, kNeighborCount };
constexpr int kOffsets[kNeighborCount][2] = {
    {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}, {2, 0}, {2, 1}, {0, 2}, {1, 2},
};

// Two colours closer than this count as "the same" (see colorDistance).
constexpr int kSameColor = 40;

inline int red(u32 c) { return (c >> 16) & 255; }
inline int green(u32 c) { return (c >> 8) & 255; }
inline int blue(u32 c) { return c & 255; }
inline u32 rgb(int r, int g, int b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }

// Blend from a to b by t/256.
inline u32 mix(u32 a, u32 b, int t) {
    const int s = 256 - t;
    return rgb((red(a) * s + red(b) * t) >> 8, (green(a) * s + green(b) * t) >> 8, (blue(a) * s + blue(b) * t) >> 8);
}

inline u32 scaleColor(u32 c, int t) {  // t/256 brightness
    return rgb((red(c) * t) >> 8, (green(c) * t) >> 8, (blue(c) * t) >> 8);
}

inline float luma(u32 c) { return 0.2126f * red(c) + 0.7152f * green(c) + 0.0722f * blue(c); }

// Runs fn(row) for rows 0..count-1, spread over all CPU cores (Grand Central
// Dispatch). Returns when every row is done.
template <typename Fn>
void parallelRows(int count, Fn&& fn) {
    constexpr int kRowsPerTask = 8;
    struct Job {
        Fn* fn;
        int count;
    } job{&fn, count};
    const std::size_t tasks = (count + kRowsPerTask - 1) / kRowsPerTask;
    dispatch_apply_f(tasks, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), &job,
                     [](void* context, std::size_t task) {
                         const Job& j = *static_cast<Job*>(context);
                         const int end = std::min(j.count, static_cast<int>(task + 1) * kRowsPerTask);
                         for (int row = static_cast<int>(task) * kRowsPerTask; row < end; ++row) (*j.fn)(row);
                     });
}

// ---- Super-xBR ----
// A 4x4 window of known pixels around a new pixel, indexed [column][row];
// the new pixel sits in the middle, between [1..2][1..2].
struct Window {
    u32 color[4][4];
    float luma[4][4];
};

// How much the brightness changes along the "/" diagonals of the window (or
// the "\" ones if kMirror). A small value means an edge runs that way.
// kDiagonal: weights for the first pass, which look at the whole window. The
// second pass only compares neighbouring pixels on the diagonals.
template <bool kDiagonal, bool kMirror>
float changeAlong(const float (&m)[4][4]) {
    auto d = [&](int x0, int y0, int x1, int y1) {
        if constexpr (kMirror) x0 = 3 - x0, x1 = 3 - x1;
        return std::abs(m[x0][y0] - m[x1][y1]);
    };
    const float side = d(0, 2, 1, 1) + d(1, 1, 2, 0) + d(1, 3, 2, 2) + d(2, 2, 3, 1);
    const float middle = d(1, 2, 2, 1);
    if constexpr (!kDiagonal) return side + 4 * middle;
    return 2 * side + 4 * middle + (d(0, 3, 1, 2) + d(2, 1, 3, 0)) - (d(0, 3, 2, 1) + d(1, 2, 3, 0)) -
           (d(0, 2, 2, 0) + d(1, 3, 3, 1)) + (d(0, 1, 1, 0) + d(2, 3, 3, 2));
}

// Picks the diagonal the edge runs along and interpolates the four pixels on
// it with a sharp 4-tap kernel (outer, inner, inner, outer). The result is
// clamped to the range of the four nearest pixels so edges never get halos.
template <bool kDiagonal>
u32 superXbrPixel(const Window& w) {
    const bool alongSlash = changeAlong<kDiagonal, false>(w.luma) <= changeAlong<kDiagonal, true>(w.luma);
    const float outer = kDiagonal ? -0.129633f : -0.175068f;
    const float inner = 0.5f - outer;
    const u32 near[4] = {w.color[1][1], w.color[2][1], w.color[1][2], w.color[2][2]};
    const u32 a = alongSlash ? w.color[0][3] : w.color[0][0], b = alongSlash ? w.color[1][2] : w.color[1][1],
              c = alongSlash ? w.color[2][1] : w.color[2][2], e = alongSlash ? w.color[3][0] : w.color[3][3];
    int out[3];
    for (int ch = 0; ch < 3; ++ch) {
        const int shift = 16 - 8 * ch;
        auto v = [shift](u32 color) { return static_cast<float>((color >> shift) & 255); };
        float lo = v(near[0]), hi = lo;
        for (u32 n : near) lo = std::min(lo, v(n)), hi = std::max(hi, v(n));
        const float value = outer * (v(a) + v(e)) + inner * (v(b) + v(c));
        out[ch] = static_cast<int>(std::clamp(value, lo, hi) + 0.5f);
    }
    return rgb(out[0], out[1], out[2]);
}

// ---- Light helpers for Retro CRT and LCD Bloom ----
// Images here are 240x160 floats, RGB interleaved.

// One box blur of the given radius across, then down (edges repeated).
void boxBlur(std::vector<float>& image, std::vector<float>& tmp, int radius) {
    auto blurLines = [radius](const float* in, float* out, int length, int count, int step, int stride) {
        const float scale = 1.0f / (2 * radius + 1);
        for (int line = 0; line < count; ++line) {
            const int base = line * stride;
            auto at = [&](int i) { return 3 * (base + std::clamp(i, 0, length - 1) * step); };
            for (int ch = 0; ch < 3; ++ch) {
                float sum = 0;
                for (int i = -radius; i <= radius; ++i) sum += in[at(i) + ch];
                for (int i = 0; i < length; ++i) {
                    out[at(i) + ch] = sum * scale;
                    sum += in[at(i + radius + 1) + ch] - in[at(i - radius) + ch];
                }
            }
        }
    };
    blurLines(image.data(), tmp.data(), kW, kH, 1, kW);  // across
    blurLines(tmp.data(), image.data(), kH, kW, kW, 1);  // down
}

// Bilinear sample at (x, y) in frame pixels; pixel i covers i..i+1.
inline void sampleLinear(const std::vector<float>& image, float x, float y, float out[3]) {
    x = std::clamp(x - 0.5f, 0.0f, kW - 1.0f);
    y = std::clamp(y - 0.5f, 0.0f, kH - 1.0f);
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, kW - 1), y1 = std::min(y0 + 1, kH - 1);
    const float fx = x - x0, fy = y - y0;
    const float* a = &image[3 * (y0 * kW + x0)];
    const float* b = &image[3 * (y0 * kW + x1)];
    const float* c = &image[3 * (y1 * kW + x0)];
    const float* d = &image[3 * (y1 * kW + x1)];
    for (int ch = 0; ch < 3; ++ch) {
        const float top = a[ch] + (b[ch] - a[ch]) * fx, bottom = c[ch] + (d[ch] - c[ch]) * fx;
        out[ch] = top + (bottom - top) * fy;
    }
}

}  // namespace

const char* videoFilterName(VideoFilter filter) {
    switch (filter) {
    case VideoFilter::PixelPerfect: return "Pixel perfect";
    case VideoFilter::SmoothHD: return "Smooth HD";
    case VideoFilter::CinematicHD: return "Cinematic HD";
    case VideoFilter::RetroLCD: return "Retro LCD";
    case VideoFilter::SuperHD: return "Super HD";
    case VideoFilter::RetroCRT: return "Retro CRT";
    case VideoFilter::LCDBloom: return "LCD Bloom";
    }
    return "?";
}

VideoProcessor::VideoProcessor() {
    const int paddedWidth = kW + 2 * kPad;
    paddedColor_.resize(paddedWidth * (kH + 2 * kPad));
    paddedYuv_.resize(paddedColor_.size());
    glow_.resize(kW * kH);
    graded_.resize(kW * kH);
    linear_.resize(kW * kH * 3);
    bloom_.resize(linear_.size());
    halo_.resize(linear_.size());
    blurTmp_.resize(linear_.size());

    constexpr double kGamma = 2.2;
    for (int i = 0; i < 256; ++i) toLinear_[i] = static_cast<float>(std::pow(i / 255.0, kGamma));
    for (int i = 0; i < kEncodeSize; ++i) {
        // Indexing by the square root gives fine steps in the darks, where
        // the eye notices them most.
        const double s = static_cast<double>(i) / (kEncodeSize - 1);
        double v = s * s * kEncodeMax;
        // Soft shoulder: values above the knee approach white gradually, so
        // bloom on bright areas doesn't turn into flat white patches.
        constexpr double kKnee = 0.75;
        if (v > kKnee) v = kKnee + (1 - kKnee) * (1 - std::exp(-(v - kKnee) / (1 - kKnee)));
        encode_[i] = static_cast<u8>(std::lround(std::pow(v, 1 / kGamma) * 255));
    }
    // A CRT draws each line with an electron beam whose brightness falls off
    // like a Gaussian above and below the line. Brighter colours make a wider
    // beam, so scanlines show clearly in dark areas and almost vanish in bright ones.
    for (int c = 0; c <= kBeamSteps; ++c) {
        const double sigma = 0.26 + 0.18 * c / kBeamSteps;  // in lines
        for (int d = 0; d <= kBeamSteps; ++d) {
            const double dist = static_cast<double>(d) / kBeamSteps;
            beam_[c][d] = static_cast<float>(std::exp(-0.5 * (dist / sigma) * (dist / sigma)));
        }
    }

    // The algorithm is written for one corner; the other three are the same
    // thing rotated by 90 degrees. Rotating (x, y) by 90 degrees: (-y, x).
    for (int k = 0; k < 4; ++k) {
        for (int n = 0; n < kNeighborCount; ++n) {
            int dx = kOffsets[n][0], dy = kOffsets[n][1];
            for (int r = 0; r < k; ++r) std::swap(dx, dy), dx = -dx;
            neighbor_[k][n] = dy * paddedWidth + dx;
        }
        // Edge shapes, in the corner's own coordinates (0..1 across pixel E,
        // the corner at (1, 1)): a 45-degree cut, a shallow and a steep one.
        // Coverage ramps over one sub-pixel so the edges are anti-aliased.
        for (int s = 0; s < 16; ++s) {
            double wx = ((s % 4) + 0.5) / 4 - 0.5, wy = ((s / 4) + 0.5) / 4 - 0.5;
            for (int r = 0; r < k; ++r) std::swap(wx, wy), wy = -wy;  // rotate back: (y, -x)
            const double x = wx + 0.5, y = wy + 0.5;
            auto cover = [](double value, double threshold, double gradient) {
                const double c = ((value - threshold) / gradient) / 0.25 + 0.5;
                return static_cast<int>(std::clamp(c, 0.0, 1.0) * 256);
            };
            coverMain_[k][s] = cover(x + y, 1.5, std::sqrt(2.0));
            coverShallow_[k][s] = cover(y + 0.5 * x, 1.0, std::sqrt(1.25));
            coverSteep_[k][s] = cover(y + 2 * x, 2.0, std::sqrt(5.0));
        }
    }
}

void VideoProcessor::process(VideoFilter filter, const u32* frame, int prescale) {
    hasGlow_ = false;
    switch (filter) {
    case VideoFilter::PixelPerfect: nearest(frame, std::clamp(prescale, 1, 8)); break;
    case VideoFilter::SmoothHD: xbr(frame); break;
    case VideoFilter::CinematicHD:
        grade(frame);
        xbr(graded_.data());
        makeGlow(graded_.data());
        hasGlow_ = true;
        break;
    case VideoFilter::RetroLCD: retroLcd(frame); break;
    case VideoFilter::SuperHD: superXbr(frame); break;
    case VideoFilter::RetroCRT: {
        // 4x to 6x (8x would take ~18 ms, too slow for 60 fps). On bigger
        // screens, prefer a scale that divides the screen's, so the GPU
        // enlarges by a whole number and every scanline stays the same height.
        int scale = std::clamp(prescale, 4, 6);
        for (int s : {6, 5, 4}) {
            if (prescale > 6 && prescale % s == 0) {
                scale = s;
                break;
            }
        }
        retroCrt(frame, scale);
        break;
    }
    case VideoFilter::LCDBloom: lcdBloom(frame); break;
    }
}

void VideoProcessor::nearest(const u32* frame, int scale) {
    width_ = kW * scale;
    height_ = kH * scale;
    image_.resize(static_cast<std::size_t>(width_) * height_);
    for (int y = 0; y < height_; ++y) {
        const u32* src = frame + (y / scale) * kW;
        u32* dst = image_.data() + static_cast<std::size_t>(y) * width_;
        for (int x = 0; x < width_; ++x) dst[x] = src[x / scale];
    }
}

// xBR ("scale by rules", by Hyllian). For each corner of each pixel it asks:
// does an edge run diagonally past this corner? It compares how similar the
// pixels are along the two diagonal directions, weighted over a 5x5 area.
// If the edge is real, the corner is cut along it and filled with the colour
// on the other side, at 45 degrees, or at a shallow/steep angle when the
// edge continues further. This turns staircase edges into smooth lines.
void VideoProcessor::xbr(const u32* frame) {
    const int paddedWidth = kW + 2 * kPad;
    for (int py = 0; py < kH + 2 * kPad; ++py) {
        const int sy = std::clamp(py - kPad, 0, kH - 1);
        for (int px = 0; px < paddedWidth; ++px) {
            const u32 c = frame[sy * kW + std::clamp(px - kPad, 0, kW - 1)];
            // Compare colours the way eyes do: brightness matters most.
            const int y = (red(c) * 299 + green(c) * 587 + blue(c) * 114) / 1000;
            paddedColor_[py * paddedWidth + px] = c;
            paddedYuv_[py * paddedWidth + px] = {y, blue(c) - y, red(c) - y};
        }
    }

    width_ = kW * kScale;
    height_ = kH * kScale;
    image_.resize(static_cast<std::size_t>(width_) * height_);

    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const int e = (y + kPad) * paddedWidth + x + kPad;
            const u32 center = paddedColor_[e];
            u32 block[16];
            std::fill(std::begin(block), std::end(block), center);

            for (int k = 0; k < 4; ++k) {
                const int* n = neighbor_[k];
                const Yuv& yE = paddedYuv_[e];
                auto at = [&](Neighbor which) -> const Yuv& { return paddedYuv_[e + n[which]]; };
                auto dist = [](const Yuv& a, const Yuv& b) {
                    return 4 * std::abs(a.y - b.y) + std::abs(a.u - b.u) + std::abs(a.v - b.v);
                };
                auto same = [&](const Yuv& a, const Yuv& b) { return dist(a, b) < kSameColor; };

                const Yuv &yB = at(B), &yC = at(C), &yD = at(D), &yF = at(F), &yG = at(G), &yH = at(H),
                          &yI = at(I), &yF4 = at(F4), &yI4 = at(I4), &yH5 = at(H5), &yI5 = at(I5);

                // Only corners where E differs from both neighbours touching it.
                if (same(yE, yF) || same(yE, yH)) continue;
                const bool allowed = (!same(yF, yB) && !same(yH, yD)) ||
                                     (same(yE, yI) && !same(yF, yI4) && !same(yH, yI5)) || same(yE, yG) ||
                                     same(yE, yC);
                if (!allowed) continue;

                // Edge running from F to H (cuts the corner) vs. from E to I.
                const int alongFH = dist(yE, yC) + dist(yE, yG) + dist(yI, yH5) + dist(yI, yF4) + 4 * dist(yH, yF);
                const int alongEI = dist(yH, yD) + dist(yH, yI5) + dist(yF, yI4) + dist(yF, yB) + 4 * dist(yE, yI);
                if (alongFH >= alongEI) continue;

                const bool shallow = 2 * dist(yF, yG) <= dist(yH, yC) && !same(yE, yG) && !same(yD, yG);
                const bool steep = dist(yF, yG) >= 2 * dist(yH, yC) && !same(yE, yC) && !same(yB, yC);
                const u32 fillColor = paddedColor_[e + (dist(yE, yF) <= dist(yE, yH) ? n[F] : n[H])];

                for (int s = 0; s < 16; ++s) {
                    int cover = coverMain_[k][s];
                    if (shallow) cover = std::max(cover, coverShallow_[k][s]);
                    if (steep) cover = std::max(cover, coverSteep_[k][s]);
                    if (cover > 0) block[s] = mix(block[s], fillColor, cover);
                }
            }

            u32* dst = image_.data() + static_cast<std::size_t>(y * kScale) * width_ + x * kScale;
            for (int sy = 0; sy < kScale; ++sy) {
                std::copy_n(block + sy * kScale, kScale, dst + static_cast<std::size_t>(sy) * width_);
            }
        }
    }
}

// Each GBA pixel becomes a 4x4 block whose right and bottom edges are
// darker, like the gaps between the cells of a handheld LCD.
void VideoProcessor::retroLcd(const u32* frame) {
    width_ = kW * kScale;
    height_ = kH * kScale;
    image_.resize(static_cast<std::size_t>(width_) * height_);
    static constexpr int kShade[4] = {256, 256, 256, 150};  // brightness per row/column in a cell
    for (int y = 0; y < height_; ++y) {
        const u32* src = frame + (y / kScale) * kW;
        u32* dst = image_.data() + static_cast<std::size_t>(y) * width_;
        const int rowShade = kShade[y % kScale];
        for (int x = 0; x < width_; ++x) {
            dst[x] = scaleColor(src[x / kScale], (rowShade * kShade[x % kScale]) >> 8);
        }
    }
}

void VideoProcessor::makeLinear(const u32* frame) {
    for (int i = 0; i < kW * kH; ++i) {
        const u32 c = frame[i];
        linear_[3 * i] = toLinear_[red(c)];
        linear_[3 * i + 1] = toLinear_[green(c)];
        linear_[3 * i + 2] = toLinear_[blue(c)];
    }
}

// Blurred copy of the frame's light. With a threshold (0..1), only pixels
// brighter than it contribute, more the brighter they are; with 0, all do.
// Several box blur passes approximate a Gaussian blur.
void VideoProcessor::makeBloom(float threshold, int radius, int passes, std::vector<float>& out) {
    for (int i = 0; i < kW * kH; ++i) {
        const float* c = &linear_[3 * i];
        float weight = 1;
        if (threshold > 0) {
            const float brightness = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            weight = std::clamp((brightness - threshold) / (1 - threshold), 0.0f, 1.0f);
        }
        for (int ch = 0; ch < 3; ++ch) out[3 * i + ch] = c[ch] * weight;
    }
    for (int pass = 0; pass < passes; ++pass) boxBlur(out, blurTmp_, radius);
}

int VideoProcessor::encode(float v) const {
    const float s = std::sqrt(std::clamp(v, 0.0f, kEncodeMax) * (1.0f / kEncodeMax));
    return encode_[static_cast<int>(s * (kEncodeSize - 1) + 0.5f)];
}

// A 90s CRT television. For each screen pixel:
//   1. Curve: bend the position outward from the centre (barrel distortion),
//      so the picture bulges like curved glass. Corners are rounded off.
//   2. Scanlines: the two nearest lines of the frame each add light by how
//      close they are, through the beam profile (beam_). Along a line the
//      colour is blended smoothly, as the beam sweeps without pixel edges.
//   3. Colour fringes: red and blue are read slightly apart, more toward the
//      left and right edges, like a TV whose beams don't quite converge.
//   4. Phosphors: columns repeat red, green, blue (an aperture grille).
//   5. Bloom: blurred bright areas spill light over lines and stripes.
//   6. Vignette: the tube is a little darker toward its edges.
void VideoProcessor::retroCrt(const u32* frame, int scale) {
    constexpr float kWarpX = 0.031f, kWarpY = 0.042f;  // curvature
    constexpr float kCorner = 7.0f;                     // corner radius, frame pixels
    constexpr float kEdgeSoftness = 0.3f;               // frame pixels
    constexpr float kFringe = 0.35f;                    // red/blue offset at the edges, frame pixels
    constexpr float kMaskBright = 1.3f, kMaskDim = 0.75f;
    constexpr float kBloom = 0.45f, kVignette = 0.22f;

    makeLinear(frame);
    makeBloom(0.12f, 3, 2, bloom_);

    width_ = kW * scale;
    height_ = kH * scale;
    image_.resize(static_cast<std::size_t>(width_) * height_);
    const int W = width_, H = height_;
    const float fringe[3] = {kFringe, 0, -kFringe};

    parallelRows(H, [&](int oy) {
        u32* dst = image_.data() + static_cast<std::size_t>(oy) * W;
        const float v = (oy + 0.5f) / H * 2 - 1;
        for (int ox = 0; ox < W; ++ox) {
            const float u = (ox + 0.5f) / W * 2 - 1;
            const float cu = u * (1 + kWarpX * v * v), cv = v * (1 + kWarpY * u * u);
            const float sx = (cu + 1) * 0.5f * kW, sy = (cv + 1) * 0.5f * kH;

            // Rounded rectangle: 1 inside, 0 outside, soft across the edge.
            const float qx = std::max(std::abs(sx - kW * 0.5f) - (kW * 0.5f - kCorner), 0.0f);
            const float qy = std::max(std::abs(sy - kH * 0.5f) - (kH * 0.5f - kCorner), 0.0f);
            const float inside = std::clamp((kCorner - std::sqrt(qx * qx + qy * qy)) / kEdgeSoftness + 0.5f, 0.0f, 1.0f);
            if (inside <= 0) {
                dst[ox] = 0xFF000000u;
                continue;
            }

            const float t = sy - 0.5f;
            const int line = static_cast<int>(std::floor(t));
            const float f = t - line;  // distance to the line above, 0..1
            const int above = std::clamp(line, 0, kH - 1) * kW, below = std::clamp(line + 1, 0, kH - 1) * kW;
            const int distAbove = static_cast<int>(f * kBeamSteps + 0.5f), distBelow = kBeamSteps - distAbove;

            float light[3];
            sampleLinear(bloom_, sx, sy, light);
            const float vignette = 1 - kVignette * (1 - (1 - cu * cu) * (1 - cv * cv));
            const int phosphor = ox % 3;
            for (int ch = 0; ch < 3; ++ch) {
                const float px = sx - 0.5f + fringe[ch] * u;
                const int left = static_cast<int>(std::floor(px));
                float fx = px - left;
                fx = fx * fx * (3 - 2 * fx);  // crisper than a plain blend, still no hard edges
                const int x0 = std::clamp(left, 0, kW - 1), x1 = std::clamp(left + 1, 0, kW - 1);
                auto along = [&](int row) {
                    const float a = linear_[3 * (row + x0) + ch], b = linear_[3 * (row + x1) + ch];
                    return a + (b - a) * fx;
                };
                auto beam = [&](float c, int dist) {
                    return c * beam_[static_cast<int>(std::min(c, 1.0f) * kBeamSteps + 0.5f)][dist];
                };
                float value = beam(along(above), distAbove) + beam(along(below), distBelow);
                value *= ch == phosphor ? kMaskBright : kMaskDim;
                value += light[ch] * kBloom;
                light[ch] = value * vignette * inside;
            }
            dst[ox] = rgb(encode(light[0]), encode(light[1]), encode(light[2]));
        }
    });
}

// Retro LCD with glowing pixels. Each cell is lit brightest in its middle,
// light from the surrounding cells bleeds into the dark grid lines (halo),
// and bright areas bloom outward over several pixels.
void VideoProcessor::lcdBloom(const u32* frame) {
    // Brightness across a cell, in linear light. The grid line matches
    // Retro LCD's 150/256 (about 0.31 in linear light).
    static constexpr float kCell[kScale] = {0.95f, 1.08f, 0.95f, 0.31f};
    constexpr float kHalo = 0.30f, kBloom = 0.45f;

    makeLinear(frame);
    makeBloom(0.0f, 1, 1, halo_);    // 3x3: neighbours' light
    makeBloom(0.10f, 4, 3, bloom_);  // wide glow from bright areas

    width_ = kW * kScale;
    height_ = kH * kScale;
    image_.resize(static_cast<std::size_t>(width_) * height_);
    const int W = width_;
    parallelRows(height_, [&](int oy) {
        u32* dst = image_.data() + static_cast<std::size_t>(oy) * W;
        const float sy = (oy + 0.5f) / kScale, rowShade = kCell[oy % kScale];
        const float* src = &linear_[3 * (oy / kScale) * kW];
        for (int ox = 0; ox < W; ++ox) {
            const float sx = (ox + 0.5f) / kScale, shade = rowShade * kCell[ox % kScale];
            float halo[3], glow[3];
            sampleLinear(halo_, sx, sy, halo);
            sampleLinear(bloom_, sx, sy, glow);
            const float* c = src + 3 * (ox / kScale);
            int out[3];
            for (int ch = 0; ch < 3; ++ch) out[ch] = encode(c[ch] * shade + halo[ch] * kHalo + glow[ch] * kBloom);
            dst[ox] = rgb(out[0], out[1], out[2]);
        }
    });
}

// Super-xBR (by Hyllian): doubles the image three times, 240x160 -> 1920x1280.
void VideoProcessor::superXbr(const u32* frame) {
    constexpr int kSteps = 3;
    int w = kW, h = kH, cur = 0;
    stepColor_[0].assign(frame, frame + kW * kH);
    stepLuma_[0].resize(kW * kH);
    for (int i = 0; i < kW * kH; ++i) stepLuma_[0][i] = luma(frame[i]);
    for (int step = 0; step < kSteps; ++step) {
        superXbrDouble(stepColor_[cur], stepLuma_[cur], w, h, stepColor_[1 - cur], stepLuma_[1 - cur]);
        cur = 1 - cur;
        w *= 2;
        h *= 2;
    }
    width_ = w;
    height_ = h;
    image_.swap(stepColor_[cur]);  // the old image buffer is reused next frame
}

// One 2x step. Old pixel (x, y) moves to (2x, 2y). The new pixels are filled
// in two passes:
//   1. (2x+1, 2y+1), the centre of each square of four old pixels.
//   2. The rest, which now sit in the centre of a diamond of known pixels:
//      the same maths on a window rotated by 45 degrees.
void VideoProcessor::superXbrDouble(const std::vector<u32>& src, const std::vector<float>& srcLuma, int w, int h,
                                    std::vector<u32>& dst, std::vector<float>& dstLuma) {
    const int W = 2 * w, H = 2 * h;
    dst.resize(static_cast<std::size_t>(W) * H);
    dstLuma.resize(dst.size());

    parallelRows(h, [&](int y) {
        int rows[4];
        for (int j = 0; j < 4; ++j) rows[j] = std::clamp(y + j - 1, 0, h - 1) * w;
        for (int x = 0; x < w; ++x) {
            const std::size_t old = static_cast<std::size_t>(2 * y) * W + 2 * x;
            dst[old] = src[y * w + x];
            dstLuma[old] = srcLuma[y * w + x];
            // Flat area: the result is clamped to the four nearest pixels, so
            // if they're all the same colour, so is the new pixel.
            const int right = std::min(x + 1, w - 1);
            const u32 nearest = src[rows[1] + x];
            if (src[rows[1] + right] == nearest && src[rows[2] + x] == nearest && src[rows[2] + right] == nearest) {
                dst[old + W + 1] = nearest;
                dstLuma[old + W + 1] = srcLuma[rows[1] + x];
                continue;
            }
            Window win;
            for (int i = 0; i < 4; ++i) {
                const int sx = std::clamp(x + i - 1, 0, w - 1);
                for (int j = 0; j < 4; ++j) {
                    win.color[i][j] = src[rows[j] + sx];
                    win.luma[i][j] = srcLuma[rows[j] + sx];
                }
            }
            const u32 c = superXbrPixel<true>(win);
            dst[old + W + 1] = c;
            dstLuma[old + W + 1] = luma(c);
        }
    });

    // Known pixels are where x + y is even. Off the edge, step back two at a
    // time so we land on a known pixel again.
    auto fold = [](int v, int max) {
        while (v < 0) v += 2;
        while (v > max) v -= 2;
        return v;
    };
    // Window column i, row j sits at offset (i + j - 3, i - j): the nearest
    // four are left, below, above and right of the new pixel.
    int offsets[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) offsets[i][j] = (i - j) * W + (i + j - 3);
    parallelRows(H, [&](int y) {
        const bool innerRow = y >= 3 && y <= H - 4;  // the window can't go off the top or bottom
        const std::size_t up = static_cast<std::size_t>(std::max(y - 1, 0)) * W,
                          down = static_cast<std::size_t>(std::min(y + 1, H - 1)) * W, row = static_cast<std::size_t>(y) * W;
        for (int x = 1 - (y & 1); x < W; x += 2) {
            const std::size_t center = row + x;
            // Flat area shortcut, as in the first pass. Off the edge, the
            // nearest known pixel is the one on the other side.
            const u32 left = dst[row + (x > 0 ? x - 1 : x + 1)];
            if (dst[row + (x < W - 1 ? x + 1 : x - 1)] == left && dst[(y > 0 ? up : down) + x] == left &&
                dst[(y < H - 1 ? down : up) + x] == left) {
                dst[center] = left;
                dstLuma[center] = dstLuma[row + (x > 0 ? x - 1 : x + 1)];
                continue;
            }
            Window win;
            if (innerRow && x >= 3 && x <= W - 4) {
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 4; ++j) {
                        win.color[i][j] = dst[center + offsets[i][j]];
                        win.luma[i][j] = dstLuma[center + offsets[i][j]];
                    }
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 4; ++j) {
                        const std::size_t at =
                            static_cast<std::size_t>(fold(y + i - j, H - 1)) * W + fold(x + i + j - 3, W - 1);
                        win.color[i][j] = dst[at];
                        win.luma[i][j] = dstLuma[at];
                    }
                }
            }
            const u32 c = superXbrPixel<false>(win);
            dst[static_cast<std::size_t>(y) * W + x] = c;
            dstLuma[static_cast<std::size_t>(y) * W + x] = luma(c);
        }
    });
}

// Richer colour: a little more saturation and contrast.
void VideoProcessor::grade(const u32* frame) {
    constexpr float kSaturation = 1.18f, kContrast = 1.08f;
    for (int i = 0; i < kW * kH; ++i) {
        const u32 c = frame[i];
        float r = red(c) / 255.0f, g = green(c) / 255.0f, b = blue(c) / 255.0f;
        const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
        auto adjust = [&](float v) {
            v = luma + (v - luma) * kSaturation;
            v = 0.5f + (v - 0.5f) * kContrast;
            return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        graded_[i] = rgb(adjust(r), adjust(g), adjust(b));
    }
}

// Soft glow: keep only the bright parts of the frame, then blur them. The
// app draws this on top with additive blending, so lights and bright
// effects seem to bleed light into their surroundings.
void VideoProcessor::makeGlow(const u32* frame) {
    static std::vector<int> r(kW * kH), g(kW * kH), b(kW * kH), tr(kW * kH), tg(kW * kH), tb(kW * kH);
    for (int i = 0; i < kW * kH; ++i) {
        const u32 c = frame[i];
        const int luma = (red(c) * 299 + green(c) * 587 + blue(c) * 114) / 1000;
        int weight = std::clamp((luma - 150) * 256 / 105, 0, 256);  // starts at ~60% brightness
        weight = weight * weight >> 8;
        r[i] = red(c) * weight >> 8;
        g[i] = green(c) * weight >> 8;
        b[i] = blue(c) * weight >> 8;
    }
    // Two box blurs in each direction approximate a Gaussian blur.
    constexpr int kRadius = 4, kTaps = 2 * kRadius + 1;
    auto blur = [](const std::vector<int>& in, std::vector<int>& out, int length, int count, int step, int stride) {
        for (int line = 0; line < count; ++line) {
            const int base = line * stride;
            int sum = 0;
            for (int i = -kRadius; i <= kRadius; ++i) sum += in[base + std::clamp(i, 0, length - 1) * step];
            for (int i = 0; i < length; ++i) {
                out[base + i * step] = sum / kTaps;
                sum += in[base + std::min(i + kRadius + 1, length - 1) * step];
                sum -= in[base + std::max(i - kRadius, 0) * step];
            }
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (auto [in, tmp] : {std::pair{&r, &tr}, std::pair{&g, &tg}, std::pair{&b, &tb}}) {
            blur(*in, *tmp, kW, kH, 1, kW);  // horizontal
            blur(*tmp, *in, kH, kW, kW, 1);  // vertical
        }
    }
    for (int i = 0; i < kW * kH; ++i) {
        glow_[i] = rgb(std::min(r[i], 255), std::min(g[i], 255), std::min(b[i], 255));
    }
}
