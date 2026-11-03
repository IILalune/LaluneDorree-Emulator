// main.cpp - LaLune Dorée Emulator's macOS frontend built on SDL2 (window, input, audio, launcher UI).
//
// The app opens on a game library. Pick a game to play it; open the pause
// menu (Esc, or the PS / mic button) to reset, go back to the library or quit.
// Settings (volume, video, controller layout, games folder...) open from the
// library (",", Create button) or the pause menu, and are saved to settings.ini.
// An empty games folder shows an "Insert a game" prompt instead of a list.
// Only the PS5 DualSense is supported as a controller (Xbox pads are not).
//
// Controls (keyboard, in game):
//   Arrow keys = D-pad    X = A    Z = B    A = L    S = R
//   Return = Start        Backspace / Right Shift = Select
//   Tab (hold) = fast-forward    F / F11 = toggle fullscreen    Esc = pause menu
//   V / Shift+V = next / previous video filter    3-6 = window size 3x-6x
//
// Controls (game controller, PS5 DualSense names; see kButtonMap):
//   Left stick / D-pad = D-pad    Cross = A    Square / Circle = B
//   L1 / L2 = L    R1 / R2 = R    Options = Start    Create = Select
//   Touchpad click (hold) = fast-forward    PS / mic button = pause menu
//
// Usage:
//   LaLuneDoree [rom.gba]                    open the library, or play a ROM directly
//   LaLuneDoree rom.gba --headless 600 --dump out.bmp [--dump-every N] [--press 300:start,...] [--save file.sav]
//                             [--filter N]  (screenshot through video filter N, 0-6; see video_filters.hpp)
//   LaLuneDoree [rom.gba] --ui-dump folder   draw every menu screen into BMP files (for checking the UI)
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gba.hpp"
#include "library.hpp"
#include "mac_ui.hpp"
#include "video_filters.hpp"
#include "settings.hpp"

namespace {

constexpr const char* kAppName = "LaLune Dor\u00E9e Emulator";

std::optional<std::vector<u8>> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<u8>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool fileExists(const std::string& path) { return static_cast<bool>(std::ifstream(path)); }

// ~/Library/Application Support/LaLuneDoree/ (saves, settings, thumbnails)
std::string prefPath() {
    char* pref = SDL_GetPrefPath("", "LaLuneDoree");
    std::string path = pref ? pref : "./";
    SDL_free(pref);
    return path;
}

// Before the rename the app kept its data in .../Kiro/KiroGBA/. On the first
// run after the rename, copy saves, settings and thumbnails across. The old
// folder is kept as a backup (unless it was empty).
void copyDataFromOldName() {
    namespace fs = std::filesystem;
    const fs::path current = prefPath();
    std::error_code ec;
    if (fs::exists(current / "settings.ini", ec)) return;  // already set up
    char* oldPref = SDL_GetPrefPath("Kiro", "KiroGBA");    // note: creates the folder if missing
    if (!oldPref) return;
    const fs::path old = oldPref;
    SDL_free(oldPref);
    for (const auto& item : fs::directory_iterator(old, ec)) {
        if (item.path().filename() == "ROMs") continue;  // games are big; they belong in games/ now
        fs::copy(item.path(), current / item.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    }
    // Don't leave an empty folder behind for people who never used the old name.
    if (fs::is_empty(old, ec)) {
        fs::remove(old, ec);
        fs::remove(old.parent_path(), ec);  // only succeeds if empty
    }
}

// Saves are named after the 4-letter game code (e.g. BLRE.sav), so a game
// keeps its save even if the ROM file is renamed. Homebrew often has no
// code, so fall back to the ROM's file name.
std::string savePathFor(const std::string& gameCode, const std::string& romPath) {
    const bool validCode = gameCode.size() == 4 &&
                           std::all_of(gameCode.begin(), gameCode.end(), [](char c) { return std::isalnum(c); });
    const std::string name = validCode ? gameCode : std::filesystem::path(romPath).stem().string();
    return prefPath() + name + ".sav";
}

u16 keyFromName(const std::string& name) {
    static const std::map<std::string, u16> kKeys = {
        {"a", KeyA}, {"b", KeyB}, {"select", KeySelect}, {"start", KeyStart},
        {"right", KeyRight}, {"left", KeyLeft}, {"up", KeyUp}, {"down", KeyDown},
        {"r", KeyR}, {"l", KeyL},
    };
    auto it = kKeys.find(name);
    return it == kKeys.end() ? 0 : it->second;
}

// Writes 0xAARRGGBB pixels as a BMP (no extra libraries needed).
bool writeBmp(const std::string& path, const u32* pixels, int width = kScreenWidth, int height = kScreenHeight) {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<u32*>(pixels), width, height, 32, width * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return false;
    const bool ok = SDL_SaveBMP(surface, path.c_str()) == 0;
    SDL_FreeSurface(surface);
    return ok;
}

// Headless mode: run N frames with scripted button presses, dump screenshots.
int runHeadless(GBA& gba, int frames, const std::string& dumpPath,
                const std::vector<std::pair<int, u16>>& presses, int dumpEvery, int filter) {
    const auto start = std::chrono::steady_clock::now();
    int peak = 0;  // loudest audio sample, to confirm sound is being produced
    for (int frame = 0; frame < frames; ++frame) {
        u16 held = 0;
        for (const auto& [at, key] : presses) {
            if (frame >= at && frame < at + 6) held |= key;  // hold each press ~0.1 s
        }
        gba.setKeys(held);
        gba.runFrame();
        for (s16 s : gba.apu.samples) peak = std::max(peak, std::abs(static_cast<int>(s)));
        gba.apu.samples.clear();
        if (dumpEvery > 0 && frame % dumpEvery == 0 && !dumpPath.empty()) {
            writeBmp(dumpPath + "." + std::to_string(frame) + ".bmp", gba.ppu.framebuffer.data());
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("Ran %d frames in %.2fs (%.1f fps, %.0f%% speed)\n", frames, seconds, frames / seconds,
                frames / seconds / 59.73 * 100.0);
    std::printf("Audio peak: %d / 32767, EEPROM address bits: %d, save dirty: %s\n", peak,
                gba.bus.eeprom.addressBits(), gba.saveDirty() ? "yes" : "no");
    if (dumpPath.empty()) return 0;
    bool ok;
    if (filter >= 0) {
        // Screenshot after a video filter.
        VideoProcessor video;
        video.process(static_cast<VideoFilter>(filter), gba.ppu.framebuffer.data(), 4);
        std::vector<u32> image = video.image();
        if (video.hasGlow()) {
            // The app draws the glow and vignette on the GPU; redo the same
            // maths here so the screenshot shows the full Cinematic HD look.
            const int w = video.width(), h = video.height();
            const auto& glow = video.glow();
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const double gx = std::clamp((x + 0.5) * kScreenWidth / w - 0.5, 0.0, kScreenWidth - 1.0);
                    const double gy = std::clamp((y + 0.5) * kScreenHeight / h - 0.5, 0.0, kScreenHeight - 1.0);
                    const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
                    const int x1 = std::min(x0 + 1, kScreenWidth - 1), y1 = std::min(y0 + 1, kScreenHeight - 1);
                    const double fx = gx - x0, fy = gy - y0;
                    const double dx = (x + 0.5) / w * 2 - 1, dy = (y + 0.5) / h * 2 - 1;
                    const double t = std::clamp((std::hypot(dx, dy) - 0.75) / 0.7, 0.0, 1.0);
                    const double keep = 1 - 0.3 * t * t * (3 - 2 * t);
                    u32& out = image[static_cast<std::size_t>(y) * w + x];
                    u32 result = 0xFF000000u;
                    for (int shift : {16, 8, 0}) {
                        auto c = [&](int px, int py) { return (glow[py * kScreenWidth + px] >> shift) & 255; };
                        const double g = (c(x0, y0) * (1 - fx) + c(x1, y0) * fx) * (1 - fy) +
                                         (c(x0, y1) * (1 - fx) + c(x1, y1) * fx) * fy;
                        const double v = std::min(255.0, ((out >> shift) & 255) + g * 140 / 255) * keep;
                        result |= static_cast<u32>(v) << shift;
                    }
                    out = result;
                }
            }
        }
        ok = writeBmp(dumpPath, image.data(), video.width(), video.height());
    } else {
        ok = writeBmp(dumpPath, gba.ppu.framebuffer.data());
    }
    if (!ok) {
        std::fprintf(stderr, "Failed to write %s\n", dumpPath.c_str());
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// UI helpers. Layout is done in window "points"; on a Retina display each
// point is 2x2 real pixels, so everything is multiplied by `scale_` when drawn.
// ---------------------------------------------------------------------------

struct Color {
    Uint8 r, g, b, a = 255;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// The look follows the app icon: a night sky with a golden pixel-art moon.
namespace palette {
constexpr Color kSkyTop{36, 32, 80};          // icon's sky, top
constexpr Color kSkyMid{18, 17, 44};          // icon's sky, bottom
constexpr Color kSkyHaze{46, 36, 90};         // soft purple glow along the bottom of the window
constexpr Color kGameBackground{8, 8, 20};    // around the game image
constexpr Color kStar{230, 230, 255};
constexpr Color kGold{245, 182, 40};          // the moon's three shades
constexpr Color kGoldLight{255, 226, 122};
constexpr Color kGoldDark{176, 112, 18};
constexpr Color kGoldShade{222, 152, 30};     // bottom bevel of gold buttons
// Panels are see-through so the sky and clouds show behind them.
constexpr Color kPanel{26, 24, 62, 196};
constexpr Color kPanelHover{46, 42, 100, 214};
constexpr Color kPanelSolid{24, 22, 58, 240};  // menus drawn over a game
constexpr Color kBorder{156, 146, 236, 70};
constexpr Color kText{242, 240, 255};
constexpr Color kTextDim{170, 166, 214};
constexpr Color kTextOnGold{40, 28, 72};
}  // namespace palette

// The library and the intro show the name as a wordmark with "EMULATOR" under it.
constexpr const char* kWordmark = "LaLune Dor\u00E9e";
constexpr const char* kTagline = "E\u2009M\u2009U\u2009L\u2009A\u2009T\u2009O\u2009R";  // thin spaces between letters

Uint8 alpha255(double a) { return static_cast<Uint8>(std::clamp(std::lround(a * 255), 0L, 255L)); }
Color withAlpha(Color c, double a) { return {c.r, c.g, c.b, alpha255(a * c.a / 255.0)}; }
double clamp01(double t) { return std::clamp(t, 0.0, 1.0); }
double smoothstep(double t) { t = clamp01(t); return t * t * (3 - 2 * t); }
u32 argb(Color c) { return (u32{c.a} << 24) | (u32{c.r} << 16) | (u32{c.g} << 8) | c.b; }
Color mix(Color a, Color b, double t) {
    auto channel = [t](Uint8 x, Uint8 y) { return static_cast<Uint8>(std::lround(x + (y - x) * t)); };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

// A tiny deterministic random generator, so the stars and clouds look the same every launch.
struct Random {
    u32 state;
    double next() {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / 16777216.0;
    }
};

enum class Align { Left, Center, Right };

// Things a player can do in a menu, from any input device.
enum class MenuAction { Up, Down, Left, Right, Accept, Back, AddRoms, OpenFolder, OpenSettings };

enum class Screen { Library, Playing, Paused, Settings };

// Rows on the settings screen, in display order.
enum class SettingRow { Volume, FastForward, VideoFilter, Scaling, Fullscreen, ConfirmButton, RomFolder, Done, Count };

std::string settingsPath() { return prefPath() + "settings.ini"; }
// Built from source: the games/ folder in the project (set by CMakeLists.txt).
// The downloadable app: a Games folder next to the saves.
std::string defaultRomFolder() {
#ifdef LALUNE_GAMES_DIR
    return LALUNE_GAMES_DIR;
#else
    return prefPath() + "Games";
#endif
}

class App {
public:
    App()
        : settings_(Settings::load(settingsPath())),
          library_(settings_.romFolder.empty() ? defaultRomFolder() : settings_.romFolder,
                   prefPath() + "Thumbnails") {}

    ~App() {
        clearTextCache();
        clearThumbnails();
        if (controller_) SDL_GameControllerClose(controller_);
        if (audio_) SDL_CloseAudioDevice(audio_);
        for (SDL_Texture* t : {screenTexture_, glowTexture_, vignetteTexture_, skyTexture_, moonTexture_,
                               moonGlowTexture_, cloudBankBack_.texture, cloudBankFront_.texture})
            if (t) SDL_DestroyTexture(t);
        for (const Sprite& cloud : clouds_)
            if (cloud.texture) SDL_DestroyTexture(cloud.texture);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
    }

    // `hidden` is for --ui-dump: no visible window, no sound.
    bool init(bool hidden = false) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // crisp nearest-neighbour pixels
        window_ = SDL_CreateWindow(kAppName, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   kScreenWidth * 4, kScreenHeight * 4,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : 0));
        if (!window_) return false;
        SDL_SetWindowMinimumSize(window_, 640, 480);
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
        if (!renderer_) return false;
        if (!createEffectTextures() || !createSceneTextures()) return false;
        startTicks_ = SDL_GetTicks();
        library_.refresh();
        if (hidden) return true;
        if (settings_.fullscreen) SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);

        SDL_AudioSpec want{}, have{};
        want.freq = APU::kSampleRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audio_) SDL_PauseAudioDevice(audio_, 0);
        else std::fprintf(stderr, "No audio device: %s\n", SDL_GetError());
        return true;
    }

    // Starts playing a ROM. On failure shows an error and stays where it was.
    bool startGame(const std::string& path) {
        auto data = readFile(path);
        auto gba = std::make_unique<GBA>();
        std::string error = data ? "" : "Could not read " + path;
        if (data && !gba->loadRom(std::move(*data), &error) && error.empty()) error = "Could not load " + path;
        if (!error.empty()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, error.c_str(), window_);
            return false;
        }
        stopGame();
        gba_ = std::move(gba);
        romPath_ = path;
        savePath_ = savePathFor(gba_->gameCode(), path);
        gba_->loadSave(savePath_);
        SDL_SetWindowTitle(window_, (std::string(kAppName) + " - " + gameName(path)).c_str());
        screen_ = Screen::Playing;
        suppressInput_ = true;
        tapped_ = 0;
        nextFrame_ = Clock::now();
        lastSave_ = Clock::now();
        return true;
    }

    void run() {
        while (running_) {
            handleEvents();
            updateScale();
            switch (screen_) {
            case Screen::Playing: stepGame(); break;
            case Screen::Paused:
                drawGameFrame();
                drawPauseMenu();
                SDL_RenderPresent(renderer_);
                SDL_Delay(16);
                break;
            case Screen::Library:
                drawLibrary();
                if (introActive()) drawIntro();
                SDL_RenderPresent(renderer_);
                SDL_Delay(16);
                break;
            case Screen::Settings:
                drawSettings();
                SDL_RenderPresent(renderer_);
                SDL_Delay(16);
                break;
            }
            // Write the save file a couple of seconds after the game changes it.
            if (gba_ && gba_->saveDirty() && Clock::now() - lastSave_ > std::chrono::seconds(2)) {
                gba_->writeSave(savePath_);
                lastSave_ = Clock::now();
            }
        }
        stopGame();
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr u32 kFastForwardFlag = 1u << 16;
    // Keep about 3 frames of audio queued: enough to avoid crackles,
    // small enough to keep latency low.
    static constexpr Uint32 kTargetAudioQueue = 3 * (APU::kSampleRate / 60) * 2 * sizeof(s16);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_AudioDeviceID audio_ = 0;

    // Video: the filtered game image, plus the extra layers of Cinematic HD.
    VideoProcessor video_;
    SDL_Texture* screenTexture_ = nullptr;  // recreated when the filter's output size changes
    int screenTextureW_ = 0, screenTextureH_ = 0;
    SDL_Texture* glowTexture_ = nullptr;     // 240x160, drawn with additive blending
    SDL_Texture* vignetteTexture_ = nullptr; // dark edges, drawn over the game
    bool frameDirty_ = true;  // the game frame or the filter changed since it was processed
    int lastPrescale_ = 0;
    std::string toast_;       // short message shown over the game, e.g. the new filter name
    Uint32 toastUntil_ = 0;
    SDL_GameController* controller_ = nullptr;
    bool running_ = true;
    float scale_ = 1.0f;  // real pixels per window point

    Screen screen_ = Screen::Library;
    Settings settings_;       // must stay above library_: it picks the library folder
    Library library_;
    Screen settingsReturn_ = Screen::Library;  // where Back goes from the settings screen
    int settingsSelected_ = 0;                 // highlighted settings row
    int selected_ = 0;        // highlighted game in the library
    int scrollTop_ = 0;       // first game row shown
    float wheelAccum_ = 0;    // trackpads send fractional scroll amounts
    int pauseSelected_ = 0;   // highlighted pause menu item
    float mouseX_ = -1, mouseY_ = -1;

    std::unique_ptr<GBA> gba_;  // the running game, if any
    std::string romPath_, savePath_;
    Clock::time_point nextFrame_ = Clock::now(), lastSave_ = Clock::now();
    bool fastForwarding_ = false;

    u32 keyboardKeys_ = 0;
    u32 controllerKeys_ = 0;
    u32 tapped_ = 0;          // keys pressed since the last frame (even if already released)
    bool suppressInput_ = false;  // ignore buttons still held from a menu until released
    bool stickActive_ = false;
    int stickSector_ = -1;    // last 8-way direction of the left stick, -1 = centred
    int navDirection_ = 0;    // controller direction held in menus (see updateMenuRepeat)
    Uint32 navRepeatAt_ = 0;  // when that held direction repeats next (ms)

    struct TextTexture {
        SDL_Texture* texture = nullptr;
        int width = 0, height = 0;  // in real pixels
    };
    std::unordered_map<std::string, TextTexture> textCache_;
    std::unordered_map<std::string, std::string> fitCache_;
    float titleFitWidth_ = -1, titleFitScale_ = 0, titleFitSize_ = 26;  // see libraryTitleSize()
    std::unordered_map<std::string, SDL_Texture*> thumbnails_;  // nullptr = no screenshot

    // The night-sky scene behind the menus (see createSceneTextures).
    struct Sprite {
        SDL_Texture* texture = nullptr;
        int w = 0, h = 0;      // in art pixels
        float x = 0, y = 0;    // floating clouds: start position (x in points, y as a fraction of the height)
        float speed = 0;       // drift in points per second
    };
    struct Star {
        float x, y;            // fractions of the window size
        bool big;              // big stars sparkle (a small plus shape)
        float speed, phase;    // twinkle
    };
    SDL_Texture* skyTexture_ = nullptr;      // vertical gradient
    SDL_Texture* moonTexture_ = nullptr;     // the icon's moon, one texel per art pixel
    SDL_Texture* moonGlowTexture_ = nullptr; // soft round glow
    int moonW_ = 0, moonH_ = 0;
    Sprite cloudBankBack_, cloudBankFront_;  // seamless strips of cloud along the bottom
    std::vector<Sprite> clouds_;             // small clouds drifting across the sky
    std::vector<Star> stars_;
    Uint32 startTicks_ = 0;
    double introSkippedAt_ = -1;  // animation time when the intro was skipped, or -1
    double timeOverride_ = -1;    // --ui-dump draws at fixed moments

    // Controller layout. SDL names face buttons by position (Xbox style):
    // A = bottom (Cross), B = right (Circle), X = left (Square), Y = top (Triangle).
    // Cross is A because it is the main action button on PlayStation; Square
    // is B because it sits left of Cross, like B sits left of A on a GBA.
    struct ButtonBinding {
        SDL_GameControllerButton button;
        u32 key;
    };
    static constexpr ButtonBinding kButtonMap[] = {
        {SDL_CONTROLLER_BUTTON_A, KeyA},                // Cross
        {SDL_CONTROLLER_BUTTON_X, KeyB},                // Square
        {SDL_CONTROLLER_BUTTON_B, KeyB},                // Circle
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, KeyL},     // L1
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, KeyR},    // R1
        {SDL_CONTROLLER_BUTTON_START, KeyStart},        // Options
        {SDL_CONTROLLER_BUTTON_BACK, KeySelect},        // Create
        {SDL_CONTROLLER_BUTTON_TOUCHPAD, kFastForwardFlag},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, KeyUp},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, KeyDown},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, KeyLeft},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, KeyRight},
    };
    static constexpr int kTriggerThreshold = 8000;  // L2/R2 count as pressed past ~25% travel
    // Radial stick deadzone with hysteresis: the stick must pass 35% to start
    // moving and drop below 25% to stop, so it doesn't flicker at the edge.
    static constexpr double kStickPress = 0.35, kStickRelease = 0.25;
    // Extra angle (radians, ~7 degrees) the stick must move past a sector
    // boundary before the direction changes. Stops jitter between straight
    // and diagonal movement.
    static constexpr double kSectorHysteresis = 0.12;

    // -----------------------------------------------------------------------
    // Running a game
    // -----------------------------------------------------------------------

    void stepGame() {
        u32 held = keyboardKeys_ | controllerKeys_;
        if (suppressInput_) {
            // E.g. Cross was pressed to pick the game: don't also send A to it.
            if (((held | tapped_) & 0x3FF) == 0) suppressInput_ = false;
            held &= kFastForwardFlag;
            tapped_ = 0;
        }
        fastForwarding_ = held & kFastForwardFlag;
        // Include buttons pressed and released since the last frame, so a
        // quick tap is never missed by the game.
        gba_->setKeys(cleanDirections(static_cast<u16>((held | tapped_) & 0x3FF)));
        tapped_ = 0;
        const int framesToRun = fastForwarding_ ? settings_.fastForwardSpeed : 1;
        for (int i = 0; i < framesToRun; ++i) gba_->runFrame();
        queueAudio();
        frameDirty_ = true;
        drawGameFrame();
        if (fastForwarding_) drawFastForwardBadge();
        drawToast();
        SDL_RenderPresent(renderer_);

        // Pace the emulator. With audio we wait until the queue drains;
        // that keeps sound smooth. Without audio we use the wall clock.
        if (!fastForwarding_) {
            if (audio_) {
                while (SDL_GetQueuedAudioSize(audio_) > kTargetAudioQueue) SDL_Delay(1);
            } else {
                nextFrame_ += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 59.7275));
                std::this_thread::sleep_until(nextFrame_);
            }
        } else {
            nextFrame_ = Clock::now();
        }
    }

    // Saves, takes a thumbnail screenshot and unloads the current game.
    void stopGame() {
        if (!gba_) return;
        gba_->writeSave(savePath_);
        writeBmp(library_.thumbnailPath(romPath_), gba_->ppu.framebuffer.data());
        forgetThumbnail(romPath_);
        gba_.reset();
        if (audio_) SDL_ClearQueuedAudio(audio_);
    }

    void pauseGame() {
        screen_ = Screen::Paused;
        pauseSelected_ = 0;
        if (audio_) SDL_ClearQueuedAudio(audio_);
        if (gba_) gba_->writeSave(savePath_);
    }

    void resumeGame() {
        screen_ = Screen::Playing;
        suppressInput_ = true;
        nextFrame_ = Clock::now();
    }

    void backToLibrary() {
        const std::string path = romPath_;
        stopGame();
        library_.refresh();
        const int index = library_.indexOf(path);
        if (index >= 0) selected_ = index;
        screen_ = Screen::Library;
        SDL_SetWindowTitle(window_, kAppName);
    }

    std::string gameName(const std::string& path) const {
        const int index = library_.indexOf(path);
        return index >= 0 ? library_.games()[index].name : std::filesystem::path(path).stem().string();
    }

    void queueAudio() {
        auto& samples = gba_->apu.samples;
        if (audio_ && !samples.empty()) {
            if (settings_.volume < 100) {
                // Integer scaling keeps this cheap; 0 gives silence.
                const int volume = settings_.volume;
                for (s16& s : samples) s = static_cast<s16>(s * volume / 100);
            }
            // While fast-forwarding, drop audio instead of building a backlog.
            if (!fastForwarding_ || SDL_GetQueuedAudioSize(audio_) < kTargetAudioQueue) {
                SDL_QueueAudio(audio_, samples.data(), static_cast<Uint32>(samples.size() * sizeof(s16)));
            }
        }
        samples.clear();
    }

    // -----------------------------------------------------------------------
    // Input
    // -----------------------------------------------------------------------

    // A real D-pad can't press opposite directions at once; some games
    // misbehave if they see that, so treat Left+Right (or Up+Down) as neither.
    static u16 cleanDirections(u16 keys) {
        if ((keys & (KeyLeft | KeyRight)) == (KeyLeft | KeyRight)) keys &= ~(KeyLeft | KeyRight);
        if ((keys & (KeyUp | KeyDown)) == (KeyUp | KeyDown)) keys &= ~(KeyUp | KeyDown);
        return keys;
    }

    // With "Circle = A" on, the right face button acts as A and Cross as B,
    // matching where A and B sit on a Nintendo pad.
    Uint8 physicalToLogical(Uint8 button) const {
        if (!settings_.circleIsA) return button;
        if (button == SDL_CONTROLLER_BUTTON_A) return SDL_CONTROLLER_BUTTON_B;
        if (button == SDL_CONTROLLER_BUTTON_B) return SDL_CONTROLLER_BUTTON_A;
        return button;
    }

    u32 keyForButton(Uint8 button) const {
        button = physicalToLogical(button);
        for (const auto& binding : kButtonMap)
            if (binding.button == button) return binding.key;
        return 0;
    }

    // Button glyphs for on-screen hints, following the confirm-button setting.
    const char* confirmGlyph() const { return settings_.circleIsA ? "\u25CB" : "\u2715"; }
    const char* backGlyph() const { return settings_.circleIsA ? "\u2715" : "\u25CB"; }

    static u32 mapKey(SDL_Keycode key) {
        switch (key) {
        case SDLK_x: return KeyA;
        case SDLK_z: return KeyB;
        case SDLK_a: return KeyL;
        case SDLK_s: return KeyR;
        case SDLK_RETURN: return KeyStart;
        case SDLK_BACKSPACE: case SDLK_RSHIFT: return KeySelect;
        case SDLK_UP: return KeyUp;
        case SDLK_DOWN: return KeyDown;
        case SDLK_LEFT: return KeyLeft;
        case SDLK_RIGHT: return KeyRight;
        case SDLK_TAB: return kFastForwardFlag;
        default: return 0;
        }
    }

    void updateController() {
        controllerKeys_ = 0;
        if (!controller_) return;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
            if (SDL_GameControllerGetButton(controller_, static_cast<SDL_GameControllerButton>(button)))
                controllerKeys_ |= keyForButton(static_cast<Uint8>(button));
        if (SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold)
            controllerKeys_ |= KeyL;
        if (SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold)
            controllerKeys_ |= KeyR;
        controllerKeys_ |= stickDirection();
    }

    // Turns the left stick into one of 8 D-pad directions. Checking each axis
    // separately (a square deadzone) makes diagonals trigger by accident;
    // splitting the circle into 8 equal 45-degree slices does not.
    u32 stickDirection() {
        static constexpr u32 kDirections[8] = {
            KeyRight, KeyRight | KeyUp, KeyUp, KeyUp | KeyLeft,
            KeyLeft, KeyLeft | KeyDown, KeyDown, KeyDown | KeyRight,
        };
        const double x = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0;
        const double y = -SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0;  // up = +
        stickActive_ = std::hypot(x, y) > (stickActive_ ? kStickRelease : kStickPress);
        if (!stickActive_) {
            stickSector_ = -1;
            return 0;
        }
        constexpr double kSlice = std::numbers::pi / 4;
        const double angle = std::atan2(y, x);
        if (stickSector_ >= 0) {
            // Stay in the current direction until the stick is clearly past the boundary.
            const double offset = std::remainder(angle - stickSector_ * kSlice, 2 * std::numbers::pi);
            if (std::abs(offset) < kSlice / 2 + kSectorHysteresis) return kDirections[stickSector_];
        }
        stickSector_ = static_cast<int>(std::lround(angle / kSlice)) & 7;
        return kDirections[stickSector_];
    }

    // Menus move one step per press, then repeat while up/down is held.
    // Vertical wins over horizontal so a slightly diagonal stick still scrolls lists.
    void updateMenuRepeat(std::vector<MenuAction>& actions) {
        int direction = 0;  // 0 = none, else 1 + index into kActions
        static constexpr MenuAction kActions[] = {MenuAction::Up, MenuAction::Down, MenuAction::Left,
                                                  MenuAction::Right};
        if (controllerKeys_ & KeyUp) direction = 1;
        else if (controllerKeys_ & KeyDown) direction = 2;
        else if (controllerKeys_ & KeyLeft) direction = 3;
        else if (controllerKeys_ & KeyRight) direction = 4;
        const Uint32 now = SDL_GetTicks();
        if (direction != navDirection_) {
            navDirection_ = direction;
            navRepeatAt_ = now + 350;
            if (direction != 0) actions.push_back(kActions[direction - 1]);
        } else if (direction != 0 && SDL_TICKS_PASSED(now, navRepeatAt_)) {
            navRepeatAt_ = now + 90;
            actions.push_back(kActions[direction - 1]);
        }
    }

    void openFirstController() {
        for (int i = 0; i < SDL_NumJoysticks() && !controller_; ++i) {
            if (SDL_IsGameController(i)) openController(i);
        }
    }

    void openController(int deviceIndex) {
        controller_ = SDL_GameControllerOpen(deviceIndex);
        if (controller_) std::printf("Controller connected: %s\n", SDL_GameControllerName(controller_));
    }

    bool isOurController(SDL_JoystickID id) const {
        return controller_ && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller_)) == id;
    }

    void handleEvents() {
        std::vector<MenuAction> actions;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Any key, click or button during the intro skips it (and does nothing else).
            if (introActive() && (event.type == SDL_KEYDOWN || event.type == SDL_MOUSEBUTTONDOWN ||
                                  event.type == SDL_CONTROLLERBUTTONDOWN)) {
                skipIntro();
                continue;
            }
            switch (event.type) {
            case SDL_QUIT: running_ = false; break;
            case SDL_KEYDOWN: onKeyDown(event.key, actions); break;
            case SDL_KEYUP: keyboardKeys_ &= ~mapKey(event.key.keysym.sym); break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (isOurController(event.cbutton.which)) onButtonDown(event.cbutton.button, actions);
                break;
            case SDL_MOUSEMOTION: onMouseMove(event.motion.x, event.motion.y); break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT) onClick(event.button.x, event.button.y, event.button.clicks);
                break;
            case SDL_MOUSEWHEEL: onWheel(event.wheel); break;
            case SDL_DROPFILE:
                onDrop(event.drop.file);
                SDL_free(event.drop.file);
                break;
            case SDL_CONTROLLERDEVICEADDED:  // `which` is a device index here
                if (!controller_) openController(event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:  // `which` is an instance ID here
                if (isOurController(event.cdevice.which)) {
                    SDL_GameControllerClose(controller_);
                    controller_ = nullptr;
                    stickActive_ = false;
                    stickSector_ = -1;
                    openFirstController();  // switch to another connected pad, if any
                }
                break;
            default: break;
            }
        }
        updateController();
        if (screen_ != Screen::Playing) updateMenuRepeat(actions);
        if (introActive()) actions.clear();
        for (MenuAction action : actions) {
            if (screen_ == Screen::Library) libraryAction(action);
            else if (screen_ == Screen::Paused) pauseAction(action);
            else if (screen_ == Screen::Settings) settingsAction(action);
        }
    }

    void onKeyDown(const SDL_KeyboardEvent& key, std::vector<MenuAction>& actions) {
        const SDL_Keycode sym = key.keysym.sym;
        if (sym == SDLK_f || sym == SDLK_F11) {
            if (!key.repeat) toggleFullscreen();
            return;
        }
        if (screen_ == Screen::Playing) {
            if (sym == SDLK_ESCAPE) {
                if (!key.repeat) pauseGame();
                return;
            }
            if (sym == SDLK_v) {  // cycle video filters (Shift+V goes backwards)
                if (!key.repeat) {
                    setVideoFilter(settings_.videoFilter + ((key.keysym.mod & KMOD_SHIFT) ? -1 : 1));
                    showToast(std::string("Filter: ") + videoFilterName(currentFilter()));
                }
                return;
            }
            if (sym >= SDLK_3 && sym <= SDLK_6) {  // smaller sizes are below the minimum window size
                if (!key.repeat) setWindowScale(3 + (sym - SDLK_3));
                return;
            }
            keyboardKeys_ |= mapKey(sym);
            if (!key.repeat) tapped_ |= mapKey(sym);
            return;
        }
        // Menus. Up/down may auto-repeat; the rest only fire once per press.
        if (sym == SDLK_UP) actions.push_back(MenuAction::Up);
        else if (sym == SDLK_DOWN) actions.push_back(MenuAction::Down);
        else if (sym == SDLK_LEFT) actions.push_back(MenuAction::Left);
        else if (sym == SDLK_RIGHT) actions.push_back(MenuAction::Right);
        if (key.repeat) return;
        switch (sym) {
        case SDLK_COMMA: actions.push_back(MenuAction::OpenSettings); break;  // also Cmd+, like other Mac apps
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: actions.push_back(MenuAction::Accept); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_z: actions.push_back(MenuAction::Back); break;
        case SDLK_o: actions.push_back(MenuAction::AddRoms); break;
        case SDLK_g: actions.push_back(MenuAction::OpenFolder); break;
        default: break;
        }
    }

    void onButtonDown(Uint8 button, std::vector<MenuAction>& actions) {
        const bool menuButton = button == SDL_CONTROLLER_BUTTON_GUIDE || button == SDL_CONTROLLER_BUTTON_MISC1;
        if (screen_ == Screen::Playing) {
            if (menuButton) pauseGame();
            else tapped_ |= keyForButton(button);
            return;
        }
        if (menuButton) {
            actions.push_back(MenuAction::Back);
            return;
        }
        switch (physicalToLogical(button)) {
        case SDL_CONTROLLER_BUTTON_BACK: actions.push_back(MenuAction::OpenSettings); break;  // Create
        case SDL_CONTROLLER_BUTTON_A: actions.push_back(MenuAction::Accept); break;        // Cross
        case SDL_CONTROLLER_BUTTON_B: actions.push_back(MenuAction::Back); break;          // Circle
        case SDL_CONTROLLER_BUTTON_Y: actions.push_back(MenuAction::AddRoms); break;       // Triangle
        case SDL_CONTROLLER_BUTTON_X: actions.push_back(MenuAction::OpenFolder); break;    // Square
        case SDL_CONTROLLER_BUTTON_START:
            actions.push_back(screen_ == Screen::Library ? MenuAction::Accept : MenuAction::Back);
            break;
        default: break;  // up/down come from updateMenuRepeat
        }
    }

    void onMouseMove(float x, float y) {
        mouseX_ = x;
        mouseY_ = y;
        if (screen_ == Screen::Settings) {
            for (int i = 0; i < static_cast<int>(SettingRow::Count); ++i)
                if (settingsRowRect(i).contains(x, y)) settingsSelected_ = i;
            return;
        }
        if (screen_ != Screen::Paused) return;
        const auto items = pauseItems();
        for (std::size_t i = 0; i < items.size(); ++i)
            if (pauseItemRect(static_cast<int>(i)).contains(x, y)) pauseSelected_ = static_cast<int>(i);
    }

    void onClick(float x, float y, int clicks) {
        if (screen_ == Screen::Paused) {
            const auto items = pauseItems();
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (pauseItemRect(static_cast<int>(i)).contains(x, y)) {
                    pauseSelected_ = static_cast<int>(i);
                    pauseAction(MenuAction::Accept);
                }
            }
            return;
        }
        if (screen_ == Screen::Settings) {
            for (int i = 0; i < static_cast<int>(SettingRow::Count); ++i) {
                if (!settingsRowRect(i).contains(x, y)) continue;
                settingsSelected_ = i;
                // The ‹ and › arrows step the value down / up; anywhere else cycles it.
                const SettingsRowParts parts = settingsRowParts(i);
                if (parts.left.contains(x, y)) settingsAction(MenuAction::Left);
                else if (parts.right.contains(x, y)) settingsAction(MenuAction::Right);
                else settingsAction(MenuAction::Accept);
            }
            return;
        }
        if (screen_ != Screen::Library) return;
        const LibraryLayout layout = libraryLayout();
        if (layout.addButton.contains(x, y)) return libraryAction(MenuAction::AddRoms);
        if (layout.folderButton.contains(x, y)) return libraryAction(MenuAction::OpenFolder);
        if (layout.settingsButton.contains(x, y)) return libraryAction(MenuAction::OpenSettings);
        if (library_.games().empty()) {
            if (emptyLibraryButton().contains(x, y)) libraryAction(MenuAction::AddRoms);
            return;
        }
        if (playButton(layout.preview).contains(x, y)) return libraryAction(MenuAction::Accept);
        if (!layout.list.contains(x, y)) return;
        const int row = scrollTop_ + static_cast<int>((y - layout.list.y) / kRowHeight);
        if (row < 0 || row >= static_cast<int>(library_.games().size())) return;
        selected_ = row;
        if (clicks >= 2) libraryAction(MenuAction::Accept);
    }

    void onWheel(const SDL_MouseWheelEvent& wheel) {
        if (screen_ != Screen::Library) return;
        float amount = wheel.preciseY;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) amount = -amount;
        wheelAccum_ += amount;
        while (wheelAccum_ >= 1) { libraryAction(MenuAction::Up); wheelAccum_ -= 1; }
        while (wheelAccum_ <= -1) { libraryAction(MenuAction::Down); wheelAccum_ += 1; }
    }

    // Dropping a .gba file on the window adds it to the library and plays it.
    void onDrop(const std::string& file) {
        std::string error;
        const auto added = library_.add(file, error);
        if (!added) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, error.c_str(), window_);
            return;
        }
        library_.refresh();
        if (const int index = library_.indexOf(*added); index >= 0) selected_ = index;
        startGame(*added);
    }

    void addRomsWithDialog() {
        const auto files = macui::chooseRomFiles();
        keyboardKeys_ = 0;  // key-up events may have gone to the dialog
        std::string errors, lastAdded;
        for (const auto& file : files) {
            std::string error;
            if (auto added = library_.add(file, error)) lastAdded = *added;
            else errors += error + "\n";
        }
        library_.refresh();
        if (const int index = library_.indexOf(lastAdded); index >= 0) selected_ = index;
        if (!errors.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, kAppName, errors.c_str(), window_);
    }

    void libraryAction(MenuAction action) {
        const int count = static_cast<int>(library_.games().size());
        switch (action) {
        case MenuAction::Up: selected_ = std::max(0, selected_ - 1); break;
        case MenuAction::Down: selected_ = std::min(count - 1, selected_ + 1); break;
        case MenuAction::Accept:
            if (selected_ >= 0 && selected_ < count) startGame(library_.games()[selected_].path);
            break;
        case MenuAction::AddRoms: addRomsWithDialog(); break;
        case MenuAction::OpenFolder: macui::openInFinder(library_.folder()); break;
        case MenuAction::OpenSettings: openSettings(); break;
        default: break;
        }
        // An empty library asks for a game: confirming opens the file picker.
        if (action == MenuAction::Accept && count == 0) addRomsWithDialog();
        selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(library_.games().size()) - 1));
    }

    std::vector<std::string> pauseItems() const {
        const bool full = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        return {"Resume", "Reset game", "Settings", full ? "Exit full screen" : "Full screen", "Back to library",
                "Quit"};
    }

    void pauseAction(MenuAction action) {
        const int count = static_cast<int>(pauseItems().size());
        switch (action) {
        case MenuAction::Up: pauseSelected_ = (pauseSelected_ + count - 1) % count; break;
        case MenuAction::Down: pauseSelected_ = (pauseSelected_ + 1) % count; break;
        case MenuAction::Back: resumeGame(); break;
        case MenuAction::Accept:
            switch (pauseSelected_) {
            case 0: resumeGame(); break;
            case 1:
                gba_->writeSave(savePath_);
                gba_->reset();
                resumeGame();
                break;
            case 2: openSettings(); break;
            case 3: toggleFullscreen(); break;
            case 4: backToLibrary(); break;
            case 5: running_ = false; break;
            }
            break;
        default: break;
        }
    }

    void toggleFullscreen() {
        const bool full = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(window_, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        settings_.fullscreen = !full;  // remembered for next launch
        saveSettings();
    }

    // -----------------------------------------------------------------------
    // Settings
    // -----------------------------------------------------------------------

    void saveSettings() {
        if (!settings_.save(settingsPath())) std::fprintf(stderr, "Could not write %s\n", settingsPath().c_str());
    }

    // Opened from the library or the pause menu; Back returns there.
    void openSettings() {
        settingsReturn_ = screen_;
        settingsSelected_ = 0;
        screen_ = Screen::Settings;
    }

    void closeSettings() {
        screen_ = settingsReturn_;
        if (screen_ == Screen::Paused) pauseSelected_ = 2;  // keep "Settings" highlighted
    }

    // Steps a value through a fixed list of choices, clamped at the ends.
    static int stepChoice(const int* choices, int count, int current, int step) {
        int index = 0;
        for (int i = 0; i < count; ++i)
            if (choices[i] == current) index = i;
        return choices[std::clamp(index + step, 0, count - 1)];
    }

    // Changes the value of a row. `step` is -1 (left), +1 (right) or 0 (accept:
    // toggle, or cycle forward and wrap around).
    void changeSetting(SettingRow row, int step) {
        switch (row) {
        case SettingRow::Volume:
            if (step == 0) settings_.volume = settings_.volume >= 100 ? 0 : std::min(100, settings_.volume + 10);
            else settings_.volume = std::clamp(settings_.volume + step * 10, 0, 100);
            break;
        case SettingRow::FastForward: {
            constexpr auto& speeds = Settings::kFastForwardSpeeds;
            constexpr int n = static_cast<int>(std::size(speeds));
            if (step == 0) {
                const int next = stepChoice(speeds, n, settings_.fastForwardSpeed, 1);
                settings_.fastForwardSpeed = next == settings_.fastForwardSpeed ? speeds[0] : next;
            } else {
                settings_.fastForwardSpeed = stepChoice(speeds, n, settings_.fastForwardSpeed, step);
            }
            break;
        }
        case SettingRow::VideoFilter: setVideoFilter(settings_.videoFilter + (step < 0 ? -1 : 1)); return;
        case SettingRow::Scaling: settings_.pixelPerfect = !settings_.pixelPerfect; break;
        case SettingRow::Fullscreen: toggleFullscreen(); return;  // saves by itself
        case SettingRow::ConfirmButton: settings_.circleIsA = !settings_.circleIsA; break;
        case SettingRow::RomFolder: {
            if (step != 0) return;
            const std::string folder = macui::chooseFolder(library_.folder());
            keyboardKeys_ = 0;  // key-up events may have gone to the dialog
            if (folder.empty() || folder == library_.folder()) return;
            library_.setFolder(folder);
            settings_.romFolder = folder == defaultRomFolder() ? "" : folder;
            selected_ = 0;
            scrollTop_ = 0;
            break;
        }
        case SettingRow::Done:
            if (step == 0) closeSettings();
            return;
        case SettingRow::Count: return;
        }
        saveSettings();
    }

    void settingsAction(MenuAction action) {
        constexpr int count = static_cast<int>(SettingRow::Count);
        const auto row = static_cast<SettingRow>(settingsSelected_);
        switch (action) {
        case MenuAction::Up: settingsSelected_ = (settingsSelected_ + count - 1) % count; break;
        case MenuAction::Down: settingsSelected_ = (settingsSelected_ + 1) % count; break;
        case MenuAction::Left: changeSetting(row, -1); break;
        case MenuAction::Right: changeSetting(row, 1); break;
        case MenuAction::Accept: changeSetting(row, 0); break;
        case MenuAction::Back: case MenuAction::OpenSettings: closeSettings(); break;
        default: break;
        }
    }

    static const char* settingLabel(SettingRow row) {
        switch (row) {
        case SettingRow::Volume: return "Volume";
        case SettingRow::FastForward: return "Fast-forward speed";
        case SettingRow::VideoFilter: return "Video filter";
        case SettingRow::Scaling: return "Screen scaling";
        case SettingRow::Fullscreen: return "Full screen";
        case SettingRow::ConfirmButton: return "Controller A button";
        case SettingRow::RomFolder: return "Games folder";
        case SettingRow::Done: return "Done";
        case SettingRow::Count: break;
        }
        return "";
    }

    // Shows paths inside the home folder as ~/... to keep them short.
    static std::string shortPath(const std::string& path) {
        const char* home = std::getenv("HOME");
        if (home && *home && path.rfind(home, 0) == 0) return "~" + path.substr(std::strlen(home));
        return path;
    }

    std::string settingValue(SettingRow row) const {
        switch (row) {
        case SettingRow::Volume: return settings_.volume == 0 ? "Muted" : std::to_string(settings_.volume) + "%";
        case SettingRow::FastForward: return std::to_string(settings_.fastForwardSpeed) + "x";
        case SettingRow::VideoFilter: return videoFilterName(currentFilter());
        case SettingRow::Scaling: return settings_.pixelPerfect ? "Whole-number" : "Fill window";
        case SettingRow::Fullscreen:
            return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? "On" : "Off";
        case SettingRow::ConfirmButton: return settings_.circleIsA ? "Circle \u25CB" : "Cross \u2715";
        case SettingRow::RomFolder: return shortPath(library_.folder());
        default: return "";
        }
    }

    // Rows that take left/right show ‹ › arrows around the value.
    static bool hasArrows(SettingRow row) {
        return row != SettingRow::RomFolder && row != SettingRow::Done;
    }

    // -----------------------------------------------------------------------
    // Drawing
    // -----------------------------------------------------------------------

    static constexpr float kRowHeight = 64;
    static constexpr double kIntroHold = 1.6;   // seconds the intro is fully shown
    static constexpr double kIntroFade = 0.55;  // then it fades into the library

    void updateScale() {
        int windowW = 0, windowH = 0, outputW = 0, outputH = 0;
        SDL_GetWindowSize(window_, &windowW, &windowH);
        SDL_GetRendererOutputSize(renderer_, &outputW, &outputH);
        const float scale = windowW > 0 ? static_cast<float>(outputW) / windowW : 1.0f;
        if (scale != scale_) {
            scale_ = scale;
            clearTextCache();  // text is rendered for one pixel density
        }
    }

    Rect windowRect() const {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        return {0, 0, static_cast<float>(w), static_cast<float>(h)};
    }

    bool hovered(const Rect& r) const { return r.contains(mouseX_, mouseY_); }

    // Fills a rectangle given in points. The edges are snapped to whole screen
    // pixels, so rectangles that touch never overlap or leave a gap.
    void fill(const Rect& r, Color c) {
        if (r.w <= 0 || r.h <= 0 || c.a == 0) return;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
        const float x0 = std::round(r.x * scale_), y0 = std::round(r.y * scale_);
        const float x1 = std::round((r.x + r.w) * scale_), y1 = std::round((r.y + r.h) * scale_);
        const SDL_FRect px{x0, y0, x1 - x0, y1 - y0};
        SDL_RenderFillRectF(renderer_, &px);
    }

    // A box with stepped, pixel-art corners: the app's panels, buttons and
    // rows. `step` is one corner step in points. The border, if any, follows
    // the same stepped outline. The pieces never overlap, so see-through
    // colours blend evenly.
    void drawPixelBox(const Rect& r, Color body, Color border = {0, 0, 0, 0}, float step = 2) {
        const float s = step, x = r.x, y = r.y, w = r.w, h = r.h;
        if (w < 4 * s || h < 4 * s) return fill(r, body);
        const Color edge = border.a ? border : body;
        fill({x + 2 * s, y + s, w - 4 * s, s}, body);
        fill({x + s, y + 2 * s, w - 2 * s, h - 4 * s}, body);
        fill({x + 2 * s, y + h - 2 * s, w - 4 * s, s}, body);
        fill({x + 2 * s, y, w - 4 * s, s}, edge);
        fill({x + 2 * s, y + h - s, w - 4 * s, s}, edge);
        fill({x, y + 2 * s, s, h - 4 * s}, edge);
        fill({x + w - s, y + 2 * s, s, h - 4 * s}, edge);
        for (float cx : {x + s, x + w - 2 * s})
            for (float cy : {y + s, y + h - 2 * s}) fill({cx, cy, s, s}, edge);
    }

    void clearTextCache() {
        for (auto& [key, text] : textCache_) SDL_DestroyTexture(text.texture);
        textCache_.clear();
        fitCache_.clear();
    }

    const TextTexture& textTexture(const std::string& text, float size, bool bold) {
        const std::string key = text + '\x1f' + std::to_string(static_cast<int>(size * 10)) + (bold ? 'b' : 'r');
        if (auto it = textCache_.find(key); it != textCache_.end()) return it->second;
        if (textCache_.size() > 512) clearTextCache();  // keep memory bounded

        TextTexture result;
        const macui::TextImage image = macui::renderText(text, size * scale_, bold);
        if (image.width > 0 && image.height > 0) {
            result.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                               image.width, image.height);
            if (result.texture) {
                SDL_UpdateTexture(result.texture, nullptr, image.pixels.data(), image.width * 4);
                SDL_SetTextureBlendMode(result.texture, SDL_BLENDMODE_BLEND);
                result.width = image.width;
                result.height = image.height;
            }
        }
        return textCache_.emplace(key, result).first->second;
    }

    float textWidth(const std::string& text, float size, bool bold) {
        return text.empty() ? 0 : textTexture(text, size, bold).width / scale_;
    }

    // Draws one line of text with its top edge at `y`. Returns its width in points.
    float drawText(const std::string& text, float x, float y, float size, Color color, bool bold = false,
                   Align align = Align::Left) {
        if (text.empty() || color.a == 0) return 0;
        const TextTexture& t = textTexture(text, size, bold);
        if (!t.texture) return 0;
        const float width = t.width / scale_;
        if (align == Align::Center) x -= width / 2;
        else if (align == Align::Right) x -= width;
        SDL_SetTextureColorMod(t.texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(t.texture, color.a);
        const SDL_Rect dst{static_cast<int>(std::lround(x * scale_)), static_cast<int>(std::lround(y * scale_)),
                           t.width, t.height};
        SDL_RenderCopy(renderer_, t.texture, nullptr, &dst);
        return width;
    }

    // Draws text vertically centred in `box`.
    void drawTextIn(const Rect& box, const std::string& text, float size, Color color, bool bold = false,
                    Align align = Align::Left, float padding = 0) {
        const TextTexture& t = textTexture(text.empty() ? " " : text, size, bold);
        const float y = box.y + (box.h - t.height / scale_) / 2;
        const float x = align == Align::Left ? box.x + padding
                        : align == Align::Right ? box.x + box.w - padding
                                                : box.x + box.w / 2;
        drawText(text, x, y, size, color, bold, align);
    }

    // Shortens text with "…" so it fits in `maxWidth` points.
    std::string fitText(const std::string& text, float size, bool bold, float maxWidth) {
        const std::string key = text + '\x1f' + std::to_string(static_cast<int>(size * 10)) + (bold ? 'b' : 'r') +
                                std::to_string(static_cast<int>(maxWidth));
        if (auto it = fitCache_.find(key); it != fitCache_.end()) return it->second;
        auto width = [&](const std::string& s) { return macui::measureText(s, size * scale_, bold) / scale_; };
        std::string result = text;
        if (width(result) > maxWidth) {
            std::string base = text;
            while (!base.empty()) {
                do base.pop_back();  // drop a whole UTF-8 character
                while (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80);
                if (width(base + "\u2026") <= maxWidth) break;
            }
            result = base + "\u2026";
        }
        return fitCache_.emplace(key, result).first->second;
    }

    // ---- Buttons, selection and key hints ----

    // A see-through button with a thin border that turns gold under the mouse.
    void drawButton(const Rect& r, const std::string& label) {
        const bool hover = hovered(r);
        drawPixelBox(r, hover ? palette::kPanelHover : palette::kPanel, hover ? palette::kGold : palette::kBorder);
        drawTextIn(r, label, 14, palette::kText, true, Align::Center);
    }

    // The main action on a screen: a gold button with a small bevel, like the moon.
    void drawGoldButton(const Rect& r, const std::string& label, float textSize, bool hover) {
        drawPixelBox(r, hover ? palette::kGoldLight : palette::kGold, palette::kGoldDark);
        fill({r.x + 2, r.y + 4, r.w - 4, 2}, hover ? Color{255, 242, 196} : palette::kGoldLight);  // top highlight
        fill({r.x + 2, r.y + r.h - 6, r.w - 4, 2}, palette::kGoldShade);                          // bottom shade
        drawTextIn(r, label, textSize, palette::kTextOnGold, true, Align::Center);
    }

    // A small ▶ made of art pixels that bobs gently, like the cursor in an old game menu.
    void drawCursor(float x, float centerY) {
        constexpr float u = 2;
        x += std::round(static_cast<float>(std::sin(animTime() * 5)) + 1);
        fill({x, centerY - 2.5f * u, u, 5 * u}, palette::kGold);
        fill({x + u, centerY - 1.5f * u, u, 3 * u}, palette::kGold);
        fill({x + 2 * u, centerY - 0.5f * u, u, u}, palette::kGold);
    }

    // The selected row of a list or menu.
    void drawSelection(const Rect& r) {
        drawPixelBox(r, palette::kPanelHover, palette::kGold);
        drawCursor(r.x + 8, r.y + r.h / 2);
    }

    struct Hint {
        std::string key, label;
    };

    // A row of key hints on a small panel, e.g. [Return] Play  [O] Add ROMs.
    // Hints that don't fit in `maxWidth` are left out, starting from the end.
    void drawHints(std::vector<Hint> hints, float x, float centerY, float maxWidth, Align align) {
        constexpr float kSize = 12, kKeyPad = 7, kGap = 7, kSpacing = 18, kPad = 12, kHeight = 36;
        auto keyWidth = [&](const Hint& h) { return textWidth(h.key, kSize, true) + 2 * kKeyPad; };
        float total = 0;
        while (!hints.empty()) {
            total = 2 * kPad - kSpacing;
            for (const Hint& h : hints) total += keyWidth(h) + kGap + textWidth(h.label, kSize, false) + kSpacing;
            if (total <= maxWidth) break;
            hints.pop_back();
        }
        if (hints.empty()) return;
        if (align == Align::Center) x -= total / 2;
        else if (align == Align::Right) x -= total;
        drawPixelBox({x, centerY - kHeight / 2, total, kHeight}, palette::kPanel, palette::kBorder);
        float cx = x + kPad;
        for (const Hint& h : hints) {
            const Rect key{cx, centerY - 11, keyWidth(h), 22};
            drawPixelBox(key, {255, 255, 255, 28}, {255, 255, 255, 46});
            drawTextIn(key, h.key, kSize, palette::kText, true, Align::Center);
            cx += key.w + kGap;
            const float labelW = textWidth(h.label, kSize, false);
            drawTextIn({cx, centerY - 11, labelW, 22}, h.label, kSize, palette::kTextDim);
            cx += labelW + kSpacing;
        }
    }

    // ---- The night sky: gradient, twinkling stars, drifting pixel clouds ----

    double animTime() const {
        return timeOverride_ >= 0 ? timeOverride_ : (SDL_GetTicks() - startTicks_) / 1000.0;
    }

    SDL_Texture* makeTexture(const std::vector<u32>& pixels, int w, int h, bool smooth) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!texture) return nullptr;
        SDL_UpdateTexture(texture, nullptr, pixels.data(), w * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        return texture;
    }

    // The moon from the app icon (tools/make_icon.py draws the same shape): a
    // circle with a smaller offset circle cut out, in three shades of gold.
    static std::vector<u32> moonArt(int& width, int& height) {
        constexpr int kGrid = 32;
        auto inside = [](int x, int y, double cx, double cy, double r) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            return dx * dx + dy * dy <= r * r;
        };
        std::vector<bool> mask(kGrid * kGrid);
        auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < kGrid && y < kGrid && mask[y * kGrid + x]; };
        for (int y = 0; y < kGrid; ++y)
            for (int x = 0; x < kGrid; ++x)
                mask[y * kGrid + x] = inside(x, y, 15.0, 16.5, 10.5) && !inside(x, y, 20.5, 12.0, 9.0);
        // Drop lone bumps: pixels touching only one other moon pixel.
        for (bool changed = true; changed;) {
            changed = false;
            std::vector<bool> next = mask;
            for (int y = 0; y < kGrid; ++y) {
                for (int x = 0; x < kGrid; ++x) {
                    if (at(x, y) && at(x + 1, y) + at(x - 1, y) + at(x, y + 1) + at(x, y - 1) <= 1) {
                        next[y * kGrid + x] = false;
                        changed = true;
                    }
                }
            }
            mask = std::move(next);
        }
        int minX = kGrid, minY = kGrid, maxX = 0, maxY = 0;
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                if (!at(x, y)) continue;
                minX = std::min(minX, x), maxX = std::max(maxX, x);
                minY = std::min(minY, y), maxY = std::max(maxY, y);
            }
        }
        width = maxX - minX + 1;
        height = maxY - minY + 1;
        std::vector<u32> pixels(static_cast<std::size_t>(width) * height, 0);
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                if (!at(x, y)) continue;
                const bool edge = !at(x + 1, y) || !at(x - 1, y) || !at(x, y + 1) || !at(x, y - 1);
                // Light comes from the upper left: the outer rim on that side is brighter.
                const bool lit = std::hypot(x + 0.5 - 15.0, y + 0.5 - 16.5) > 8.0 && x + y < 29;
                pixels[(y - minY) * width + (x - minX)] =
                    argb(edge ? palette::kGoldDark : lit ? palette::kGoldLight : palette::kGold);
            }
        }
        return pixels;
    }

    // A pixel-art cloud: round puffs on a flat base, lit from above, with a
    // dithered shadow underneath. `wrap` joins the left and right edges so a
    // long strip can repeat seamlessly.
    static std::vector<u32> cloudArt(Random& rng, int w, int h, bool wrap, Color body, Color light, Color shade) {
        struct Puff {
            double x, y, r;
        };
        std::vector<Puff> puffs;
        double slabTop;
        if (wrap) {
            // Overlapping round puffs, some with a smaller puff on top, so the
            // edge is a row of soft bumps (not hills with deep valleys).
            slabTop = h * 0.5;
            for (double x = 0; x < w - 5; x += 7 + rng.next() * 4) {
                const double r = 8 + rng.next() * 5;
                const double y = slabTop + 4 + rng.next() * 2;
                puffs.push_back({x, y, r});
                if (rng.next() < 0.3) puffs.push_back({x + (rng.next() - 0.5) * r, y - r * 0.7, r * 0.6});
            }
        } else {
            slabTop = h - 7;
            puffs.push_back({5, h - 5.0, 4});
            puffs.push_back({w - 5.0, h - 5.0, 4});
            for (int k = 1; k <= 3; ++k)
                puffs.push_back({w * k / 4.0 + (rng.next() - 0.5) * 4, h - 5.0,
                                 h * (k == 2 ? 0.6 : 0.4 + rng.next() * 0.12)});
        }
        auto inside = [&](int x, int y) {
            if (wrap) x = (x % w + w) % w;
            else if (x < 0 || x >= w) return false;
            if (y < 0) return false;
            if (y >= h) return wrap;  // the strip carries on below the window's edge
            const bool slab = y >= slabTop && (wrap || (x >= 4 && x < w - 4 && y < h - 2));
            if (slab) return true;
            for (const Puff& p : puffs) {
                double dx = std::abs(x + 0.5 - p.x);
                if (wrap) dx = std::min(dx, w - dx);
                const double dy = y + 0.5 - p.y;
                if (dx * dx + dy * dy <= p.r * p.r) return true;
            }
            return false;
        };
        const int shadeFrom = wrap ? static_cast<int>(slabTop) + 8 : h - 4;
        std::vector<u32> pixels(static_cast<std::size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!inside(x, y)) continue;
                Color c = body;
                if (!inside(x, y - 1) || !inside(x, y - 2)) c = light;           // sunlit rim
                else if (!inside(x, y + 1)) c = shade;                           // underside
                else if (y >= shadeFrom + 2) c = shade;                          // shadow below...
                else if (y >= shadeFrom && (x + y) % 2 == 0) c = shade;          // ...with a dithered edge
                pixels[static_cast<std::size_t>(y) * w + x] = argb(c);
            }
        }
        return pixels;
    }

    bool createSceneTextures() {
        // Sky: the icon's gradient, with a purple haze rising from the clouds.
        std::vector<u32> sky(256);
        for (int i = 0; i < 256; ++i) {
            const double t = i / 255.0;
            Color c = mix(palette::kSkyTop, palette::kSkyMid, smoothstep(t / 0.75));
            if (t > 0.55) c = mix(c, palette::kSkyHaze, 0.85 * smoothstep((t - 0.55) / 0.45));
            sky[i] = argb(c);
        }
        skyTexture_ = makeTexture(sky, 1, 256, true);

        // Glow: white, fading out from the centre (tinted gold when drawn).
        constexpr int kGlow = 128;
        std::vector<u32> glow(kGlow * kGlow);
        for (int y = 0; y < kGlow; ++y) {
            for (int x = 0; x < kGlow; ++x) {
                const double d = std::hypot(x + 0.5 - kGlow / 2.0, y + 0.5 - kGlow / 2.0) / (kGlow / 2.0);
                glow[y * kGlow + x] = argb(withAlpha({255, 255, 255}, std::pow(clamp01(1 - d), 2.2)));
            }
        }
        moonGlowTexture_ = makeTexture(glow, kGlow, kGlow, true);

        const std::vector<u32> moon = moonArt(moonW_, moonH_);
        moonTexture_ = makeTexture(moon, moonW_, moonH_, false);

        Random rng{20261103};
        auto bank = [&](int w, int h, Color body, Color light, Color shade) {
            Sprite s;
            s.w = w;
            s.h = h;
            s.texture = makeTexture(cloudArt(rng, w, h, true, body, light, shade), w, h, false);
            return s;
        };
        cloudBankBack_ = bank(200, 44, {70, 58, 140}, {104, 90, 180}, {54, 44, 114});
        cloudBankFront_ = bank(160, 40, {116, 100, 192}, {172, 158, 236}, {84, 70, 158});
        for (int i = 0; i < 6; ++i) {
            Sprite s;
            s.w = 30 + static_cast<int>(rng.next() * 18);
            s.h = 14 + static_cast<int>(rng.next() * 4);
            s.x = static_cast<float>(rng.next() * 1600);
            s.y = static_cast<float>(0.08 + rng.next() * 0.42);
            s.speed = static_cast<float>(3 + rng.next() * 7);
            s.texture = makeTexture(cloudArt(rng, s.w, s.h, false, {96, 84, 174}, {148, 136, 222}, {70, 60, 142}),
                                    s.w, s.h, false);
            clouds_.push_back(s);
        }

        for (int i = 0; i < 120; ++i) {
            Star s;
            s.x = static_cast<float>(rng.next());
            s.y = static_cast<float>(std::pow(rng.next(), 1.3) * 0.74);  // more stars higher up
            s.big = rng.next() < 0.12;
            s.speed = static_cast<float>(0.8 + rng.next() * 2.2);
            s.phase = static_cast<float>(rng.next() * 2 * std::numbers::pi);
            stars_.push_back(s);
        }
        return skyTexture_ && moonGlowTexture_ && moonTexture_ && cloudBankBack_.texture && cloudBankFront_.texture;
    }

    // Size in points of `cells` art pixels drawn `unit` points wide (rounded to whole screen pixels).
    float artSize(int cells, float unit) const {
        return static_cast<float>(std::max(1L, std::lround(unit * scale_)) * cells) / scale_;
    }

    // Draws pixel art with each art pixel `unit` points wide, kept crisp.
    void drawArt(SDL_Texture* texture, int w, int h, float x, float y, float unit, double alpha = 1) {
        if (!texture || alpha <= 0) return;
        const int px = static_cast<int>(std::max(1L, std::lround(unit * scale_)));
        const SDL_Rect dst{static_cast<int>(std::lround(x * scale_)), static_cast<int>(std::lround(y * scale_)),
                           w * px, h * px};
        SDL_SetTextureAlphaMod(texture, alpha255(alpha));
        SDL_RenderCopy(renderer_, texture, nullptr, &dst);
    }

    void drawMoon(float x, float y, float unit, double alpha = 1) {
        drawArt(moonTexture_, moonW_, moonH_, x, y, unit, alpha);
    }

    // A soft gold glow centred on (cx, cy), `size` points across.
    void drawGlow(float cx, float cy, float size, double alpha) {
        if (alpha <= 0) return;
        SDL_SetTextureColorMod(moonGlowTexture_, palette::kGold.r, palette::kGold.g, palette::kGold.b);
        SDL_SetTextureAlphaMod(moonGlowTexture_, alpha255(alpha));
        const SDL_Rect dst{static_cast<int>((cx - size / 2) * scale_), static_cast<int>((cy - size / 2) * scale_),
                           static_cast<int>(size * scale_), static_cast<int>(size * scale_)};
        SDL_RenderCopy(renderer_, moonGlowTexture_, nullptr, &dst);
    }

    // Repeats a cloud strip across the window, drifting right.
    void drawCloudBank(const Sprite& bank, float top, float speed, double alpha) {
        if (alpha <= 0) return;
        int outW = 0, outH = 0;
        SDL_GetRendererOutputSize(renderer_, &outW, &outH);
        // Work in whole screen pixels so the copies always meet exactly.
        const int cell = static_cast<int>(std::max(1L, std::lround(4 * scale_)));
        const int w = bank.w * cell;
        const int offset = static_cast<int>(std::fmod(animTime() * speed * scale_, w));
        const int y = static_cast<int>(std::lround(top * scale_));
        SDL_SetTextureAlphaMod(bank.texture, alpha255(alpha));
        for (int x = offset - w; x < outW; x += w) {
            const SDL_Rect dst{x, y, w, bank.h * cell};
            SDL_RenderCopy(renderer_, bank.texture, nullptr, &dst);
        }
    }

    // The background of the menus. `alpha` below 1 draws it see-through (the
    // intro fading out). No stars are drawn in `keepClear`, so none sit on the
    // title and look like an accent on a letter.
    void drawScene(double alpha = 1, const Rect& keepClear = {}) {
        const Rect win = windowRect();
        const double t = animTime();
        int outW = 0, outH = 0;
        SDL_GetRendererOutputSize(renderer_, &outW, &outH);
        SDL_SetTextureAlphaMod(skyTexture_, alpha255(alpha));
        const SDL_Rect all{0, 0, outW, outH};
        SDL_RenderCopy(renderer_, skyTexture_, nullptr, &all);

        for (const Star& s : stars_) {
            const double twinkle = 0.3 + 0.7 * (0.5 + 0.5 * std::sin(t * s.speed + s.phase));
            const float x = std::round(s.x * win.w / 2) * 2, y = std::round(s.y * win.h / 2) * 2;  // 2-point grid
            if (keepClear.contains(x, y)) continue;
            fill({x, y, 2, 2}, withAlpha(palette::kStar, alpha * twinkle));
            if (s.big) {  // a little plus-shaped sparkle
                const Color arm = withAlpha(palette::kStar, alpha * twinkle * 0.45);
                fill({x - 2, y, 2, 2}, arm);
                fill({x + 2, y, 2, 2}, arm);
                fill({x, y - 2, 2, 2}, arm);
                fill({x, y + 2, 2, 2}, arm);
            }
        }

        for (const Sprite& c : clouds_) {
            const float w = artSize(c.w, 4);
            const float x = static_cast<float>(std::fmod(c.x + t * c.speed, win.w + w)) - w;
            drawArt(c.texture, c.w, c.h, x, std::round(c.y * win.h), 4, alpha * 0.5);
        }
        // The back strip drifts slower than the front one, which gives depth.
        drawCloudBank(cloudBankBack_, win.h - 160, 5, alpha * 0.85);
        drawCloudBank(cloudBankFront_, win.h - 104, 11, alpha * 0.95);
    }

    // ---- Launch intro ----

    // Time along the intro. Skipping jumps to the fade-out.
    double introClock() const {
        const double t = animTime();
        return introSkippedAt_ < 0 ? t : kIntroHold + (t - introSkippedAt_);
    }

    bool introActive() const { return screen_ == Screen::Library && introClock() < kIntroHold + kIntroFade; }

    void skipIntro() {
        if (introSkippedAt_ < 0 && animTime() < kIntroHold) introSkippedAt_ = animTime();
    }

    // The moon rises above the clouds, the name appears under it, and the
    // whole scene fades into the library (drawn underneath).
    void drawIntro() {
        const double clock = introClock();
        const double t = introSkippedAt_ < 0 ? clock : introSkippedAt_;  // skipping freezes the animation
        const double a = 1 - smoothstep((clock - kIntroHold) / kIntroFade);
        const Rect win = windowRect();
        const float unit = win.h >= 600 ? 6 : 5;
        const float mw = artSize(moonW_, unit), mh = artSize(moonH_, unit);
        const float titleSize = win.w >= 800 ? 44 : 36;
        const float top = (win.h - (mh + 24 + titleSize * 1.3f + 24)) / 2 - 24;
        const float textW = textWidth(kWordmark, titleSize, true);
        drawScene(a, {(win.w - textW) / 2 - 16, top + mh + 16, textW + 32, titleSize * 1.3f + 40});

        const double rise = smoothstep(t / 1.1);
        const float moonX = (win.w - mw) / 2, moonY = top + static_cast<float>((1 - rise) * 36);
        const double pulse = 0.85 + 0.15 * std::sin(animTime() * 2.2);
        drawGlow(moonX + mw / 2, moonY + mh / 2, mw * 3.4f, a * rise * 0.4 * pulse);
        drawMoon(moonX, moonY, unit, a * smoothstep(t / 0.7));

        const double title = smoothstep((t - 0.45) / 0.5);
        const float titleY = top + mh + 24 + static_cast<float>((1 - title) * 10);
        drawText(kWordmark, win.w / 2, titleY, titleSize, withAlpha(palette::kGoldLight, a * title), true,
                 Align::Center);
        const double tag = smoothstep((t - 0.75) / 0.5);
        drawText(kTagline, win.w / 2, titleY + titleSize * 1.3f, 13, withAlpha(palette::kTextDim, a * tag), true,
                 Align::Center);
    }

    // ---- Playing ----

    // Where the game image goes, centred, in real pixels: the biggest
    // whole-number multiple of 240x160 that fits, or (Fill window) the
    // biggest 3:2 rectangle that fits.
    SDL_Rect gameRect() const {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        const int scale = std::min(w / kScreenWidth, h / kScreenHeight);
        int gw = kScreenWidth * scale, gh = kScreenHeight * scale;
        if (scale < 1 || !settings_.pixelPerfect) {  // fill mode or tiny window: largest 3:2 fit
            gw = std::min(w, h * kScreenWidth / kScreenHeight);
            gh = gw * kScreenHeight / kScreenWidth;
        }
        return {(w - gw) / 2, (h - gh) / 2, gw, gh};
    }

    VideoFilter currentFilter() const { return static_cast<VideoFilter>(settings_.videoFilter); }

    bool createEffectTextures() {
        glowTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         kScreenWidth, kScreenHeight);
        if (!glowTexture_) return false;
        SDL_SetTextureBlendMode(glowTexture_, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(glowTexture_, SDL_ScaleModeLinear);  // the upscale itself softens the glow
        SDL_SetTextureAlphaMod(glowTexture_, 140);                   // glow strength

        // Vignette: transparent in the middle, darkening smoothly toward the edges.
        constexpr int kSize = 256;
        std::vector<u32> pixels(kSize * kSize);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const double dx = (x + 0.5) / kSize * 2 - 1, dy = (y + 0.5) / kSize * 2 - 1;
                const double t = std::clamp((std::hypot(dx, dy) - 0.75) / 0.7, 0.0, 1.0);
                const double alpha = 0.3 * t * t * (3 - 2 * t);  // smoothstep
                pixels[y * kSize + x] = static_cast<u32>(alpha * 255) << 24;  // black with alpha
            }
        }
        vignetteTexture_ =
            SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
        if (!vignetteTexture_) return false;
        SDL_UpdateTexture(vignetteTexture_, nullptr, pixels.data(), kSize * 4);
        SDL_SetTextureBlendMode(vignetteTexture_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(vignetteTexture_, SDL_ScaleModeLinear);
        return true;
    }

    // Runs the video filter on the current game frame and uploads the result.
    void processFrame(const SDL_Rect& dst) {
        // Pixel perfect enlarges pixels on the CPU by the whole-number part of
        // the screen scale; the GPU's smooth scaling then only has to cover
        // the remainder, which keeps every pixel the same size.
        const int prescale = std::max(1, dst.w / kScreenWidth);
        if (!frameDirty_ && prescale == lastPrescale_) return;
        video_.process(currentFilter(), gba_->ppu.framebuffer.data(), prescale);
        lastPrescale_ = prescale;
        frameDirty_ = false;

        if (!screenTexture_ || screenTextureW_ != video_.width() || screenTextureH_ != video_.height()) {
            if (screenTexture_) SDL_DestroyTexture(screenTexture_);
            screenTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                               video_.width(), video_.height());
            if (!screenTexture_) return;
            SDL_SetTextureScaleMode(screenTexture_, SDL_ScaleModeLinear);
            screenTextureW_ = video_.width();
            screenTextureH_ = video_.height();
        }
        SDL_UpdateTexture(screenTexture_, nullptr, video_.image().data(), video_.width() * sizeof(u32));
        if (video_.hasGlow()) {
            SDL_UpdateTexture(glowTexture_, nullptr, video_.glow().data(), kScreenWidth * sizeof(u32));
        }
    }

    void drawGameFrame() {
        const Color bg = palette::kGameBackground;
        SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(renderer_);
        if (!gba_) return;
        const SDL_Rect dst = gameRect();
        processFrame(dst);
        if (!screenTexture_) return;
        SDL_RenderCopy(renderer_, screenTexture_, nullptr, &dst);
        if (video_.hasGlow()) {
            SDL_RenderCopy(renderer_, glowTexture_, nullptr, &dst);
            SDL_RenderCopy(renderer_, vignetteTexture_, nullptr, &dst);
        }
    }

    // Shown in the corner while Tab or the touchpad is held.
    void drawFastForwardBadge() {
        const Rect box{14, 12, 52, 30};
        drawPixelBox(box, palette::kPanelSolid, withAlpha(palette::kGold, 0.6));
        drawTextIn(box, "\u25B6\u25B6", 13, palette::kGoldLight, true, Align::Center);
    }

    void setVideoFilter(int filter) {
        settings_.videoFilter = (filter + kVideoFilterCount) % kVideoFilterCount;
        frameDirty_ = true;
        saveSettings();
    }

    void showToast(const std::string& text) {
        toast_ = text;
        toastUntil_ = SDL_GetTicks() + 1500;
    }

    void drawToast() {
        if (toast_.empty() || SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_)) return;
        const Rect win = windowRect();
        const float w = textWidth(toast_, 15, true) + 40;
        const Rect box{(win.w - w) / 2, 18, w, 40};
        drawPixelBox(box, palette::kPanelSolid, palette::kBorder);
        drawTextIn(box, toast_, 15, palette::kText, true, Align::Center);
    }

    // Keys 3-6 resize the window so the game is shown at that many times its size.
    void setWindowScale(int times) {
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) toggleFullscreen();
        SDL_SetWindowSize(window_, kScreenWidth * times, kScreenHeight * times);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        showToast("Window size " + std::to_string(times) + "x");
    }

    // ---- Menu panels (pause menu and settings) ----

    // A panel's title with a small moon in front of it.
    void drawMenuTitle(const Rect& panel, const std::string& title) {
        const float tw = textWidth(title, 22, true);
        const float mw = artSize(moonW_, 1), mh = artSize(moonH_, 1);
        const float x = panel.x + (panel.w - (mw + 10 + tw)) / 2, cy = panel.y + 34;
        drawMoon(x, cy - mh / 2, 1);
        drawTextIn({x + mw + 10, cy - 18, tw, 36}, title, 22, palette::kText, true);
    }

    // Behind a menu opened while playing: the game, dimmed with a night-blue tint.
    void dimGame() {
        drawGameFrame();
        fill(windowRect(), {6, 6, 22, 178});
    }

    // ---- Pause menu ----

    Rect pausePanel() const {
        const Rect win = windowRect();
        const float w = 360, h = 70 + pauseItems().size() * 46.0f + 52;
        return {(win.w - w) / 2, (win.h - h) / 2, w, h};
    }

    Rect pauseItemRect(int i) const {
        const Rect panel = pausePanel();
        return {panel.x + 18, panel.y + 64 + i * 46.0f, panel.w - 36, 40};
    }

    void drawPauseMenu() {
        dimGame();
        const Rect panel = pausePanel();
        drawPixelBox(panel, palette::kPanelSolid, palette::kBorder, 3);
        drawMenuTitle(panel, "Paused");
        const auto items = pauseItems();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Rect r = pauseItemRect(static_cast<int>(i));
            const bool selected = static_cast<int>(i) == pauseSelected_;
            if (selected) drawSelection(r);
            drawTextIn(r, items[i], 16, selected ? palette::kText : palette::kTextDim, selected, Align::Center);
        }
        const std::vector<Hint> hints = controller_
                                            ? std::vector<Hint>{{confirmGlyph(), "Select"}, {backGlyph(), "Resume"}}
                                            : std::vector<Hint>{{"Return", "Select"}, {"Esc", "Resume"}};
        drawHints(hints, panel.x + panel.w / 2, panel.y + panel.h - 30, panel.w - 24, Align::Center);
    }

    // ---- Library ----

    struct LibraryLayout {
        Rect addButton, folderButton, settingsButton, list, preview, footer;
        int visibleRows = 1;
    };

    LibraryLayout libraryLayout() const {
        const Rect win = windowRect();
        LibraryLayout l;
        // Sized so the three buttons still clear the title at the 640-point minimum width.
        l.addButton = {win.w - 32 - 118, 30, 118, 36};
        l.folderButton = {l.addButton.x - 10 - 116, 30, 116, 36};
        l.settingsButton = {l.folderButton.x - 10 - 92, 30, 92, 36};
        const float top = 126, bottom = win.h - 64;
        const float listW = std::max(280.0f, (win.w - 64 - 24) * 0.46f);
        l.list = {32, top, listW, bottom - top};
        l.preview = {32 + listW + 24, top - 26, win.w - 32 - (32 + listW + 24), bottom - top + 26};
        l.footer = {32, bottom, win.w - 64, 64};
        l.visibleRows = std::max(1, static_cast<int>(l.list.h / kRowHeight));
        return l;
    }

    void clearThumbnails() {
        for (auto& [path, texture] : thumbnails_)
            if (texture) SDL_DestroyTexture(texture);
        thumbnails_.clear();
    }

    void forgetThumbnail(const std::string& romPath) {
        if (auto it = thumbnails_.find(romPath); it != thumbnails_.end()) {
            if (it->second) SDL_DestroyTexture(it->second);
            thumbnails_.erase(it);
        }
    }

    SDL_Texture* thumbnail(const std::string& romPath) {
        if (auto it = thumbnails_.find(romPath); it != thumbnails_.end()) return it->second;
        SDL_Texture* texture = nullptr;
        if (SDL_Surface* surface = SDL_LoadBMP(library_.thumbnailPath(romPath).c_str())) {
            texture = SDL_CreateTextureFromSurface(renderer_, surface);
            SDL_FreeSurface(surface);
        }
        thumbnails_[romPath] = texture;
        return texture;
    }

    // A game's last screenshot in `r`, or a dim moon if it hasn't been played yet.
    void drawThumbnail(const Rect& r, const std::string& romPath, float moonUnit) {
        if (SDL_Texture* image = thumbnail(romPath)) {
            const SDL_Rect dst{static_cast<int>(std::lround(r.x * scale_)), static_cast<int>(std::lround(r.y * scale_)),
                               static_cast<int>(std::lround(r.w * scale_)), static_cast<int>(std::lround(r.h * scale_))};
            SDL_RenderCopy(renderer_, image, nullptr, &dst);
            return;
        }
        fill(r, {10, 10, 30, 230});
        const float mw = artSize(moonW_, moonUnit), mh = artSize(moonH_, moonUnit);
        drawMoon(r.x + (r.w - mw) / 2, r.y + (r.h - mh) / 2, moonUnit, 0.45);
    }

    // Font size for the library title: 26pt, shrinking (down to 16pt) in narrow
    // windows so it clears the buttons. Cached because measuring uses CoreText.
    float libraryTitleSize(float maxWidth) {
        if (maxWidth != titleFitWidth_ || scale_ != titleFitScale_) {
            titleFitWidth_ = maxWidth;
            titleFitScale_ = scale_;
            titleFitSize_ = 26;
            while (titleFitSize_ > 16 && macui::measureText(kWordmark, titleFitSize_ * scale_, true) / scale_ > maxWidth)
                titleFitSize_ -= 1;
        }
        return titleFitSize_;
    }

    void drawLibrary() {
        const auto& games = library_.games();
        const int count = static_cast<int>(games.size());
        const LibraryLayout l = libraryLayout();
        selected_ = std::clamp(selected_, 0, std::max(0, count - 1));

        // Header: the moon and the name, like the app icon.
        const float logoW = artSize(moonW_, 2), logoH = artSize(moonH_, 2);
        const float logoY = 28;
        const float titleX = 32 + logoW + 14;
        const float titleMaxWidth = l.settingsButton.x - 16 - titleX;
        const float titleSize = libraryTitleSize(titleMaxWidth);
        drawScene(1, {24, 16, titleX + textWidth(kWordmark, titleSize, true) + 12 - 24, 66});

        drawGlow(32 + logoW / 2, logoY + logoH / 2, logoW * 3, 0.16);
        drawMoon(32, logoY, 2);
        drawText(fitText(kWordmark, titleSize, true, titleMaxWidth), titleX, 22 + (26 - titleSize) / 2, titleSize,
                 palette::kGoldLight, true);
        drawText(kTagline, titleX + 1, 58, 10, palette::kTextDim, true);
        drawButton(l.settingsButton, "Settings");
        drawButton(l.folderButton, "Open Folder");
        drawGoldButton(l.addButton, "Add ROMs\u2026", 14, hovered(l.addButton));

        if (count == 0) {
            drawInsertGamePrompt();
        } else {
            drawText("L\u2009I\u2009B\u2009R\u2009A\u2009R\u2009Y", l.list.x + 2, l.list.y - 25, 11, palette::kTextDim,
                     true);
            drawText(count == 1 ? "1 game" : std::to_string(count) + " games", l.list.x + l.list.w - 12,
                     l.list.y - 26, 12, palette::kTextDim, false, Align::Right);
            drawGameList(l);
            drawPreview(l.preview, games[selected_]);
        }

        const std::string play = count == 0 ? "Insert game" : "Play";
        const std::vector<Hint> hints =
            controller_ ? std::vector<Hint>{{confirmGlyph(), play}, {"\u25B3", "Add ROMs"}, {"\u25A1", "Open folder"},
                                            {"Create", "Settings"}}
                        : std::vector<Hint>{{"Return", play}, {"O", "Add ROMs"}, {"G", "Open folder"},
                                            {",", "Settings"}, {"\u2318Q", "Quit"}};
        drawHints(hints, l.footer.x, l.footer.y + l.footer.h / 2 - 2, l.footer.w, Align::Left);
    }

    // ---- Empty library: ask for a game ----

    Rect emptyCard() const {
        const Rect win = windowRect();
        const float w = std::min(460.0f, win.w - 64), h = 290;
        const float top = 96, bottom = win.h - 64;
        return {(win.w - w) / 2, top + std::max(0.0f, (bottom - top - h) / 2), w, h};
    }

    Rect emptyLibraryButton() const {
        const Rect card = emptyCard();
        return {card.x + (card.w - 200) / 2, card.y + card.h - 26 - 44, 200, 44};
    }

    void drawInsertGamePrompt() {
        const Rect card = emptyCard();
        drawPixelBox(card, palette::kPanel, palette::kBorder, 3);
        const float cx = card.x + card.w / 2;
        const float mw = artSize(moonW_, 3), mh = artSize(moonH_, 3);
        float y = card.y + 26;
        drawGlow(cx, y + mh / 2, mw * 3, 0.22);
        drawMoon(cx - mw / 2, y, 3);
        y += mh + 18;
        drawText("Insert a game to play", cx, y, 22, palette::kText, true, Align::Center);
        y += 34;
        drawText("Choose a .gba file, or drag one onto this window.", cx, y, 14, palette::kTextDim, false,
                 Align::Center);
        y += 22;
        drawText("Games are kept in " + fitText(shortPath(library_.folder()), 13, false, card.w - 170), cx, y, 13,
                 withAlpha(palette::kTextDim, 0.8), false, Align::Center);
        // Gold: it is the only thing to do on this screen.
        const Rect button = emptyLibraryButton();
        drawGoldButton(button, "Insert Game\u2026", 16, hovered(button));
    }

    // ---- Settings ----

    struct SettingsRowParts {
        Rect left, value, right;
    };

    Rect settingsPanel() const {
        const Rect win = windowRect();
        const float w = std::min(580.0f, win.w - 48);
        const float h = 70 + static_cast<int>(SettingRow::Count) * 42.0f + 52;
        return {(win.w - w) / 2, (win.h - h) / 2, w, h};
    }

    Rect settingsRowRect(int i) const {
        const Rect panel = settingsPanel();
        return {panel.x + 18, panel.y + 64 + i * 42.0f, panel.w - 36, 38};
    }

    // The value sits in the right half of a row, between ‹ and › click targets.
    SettingsRowParts settingsRowParts(int i) const {
        const Rect row = settingsRowRect(i);
        const float valueW = row.w * 0.5f;
        const float x = row.x + row.w - valueW;
        if (!hasArrows(static_cast<SettingRow>(i))) return {{}, {x, row.y, valueW - 14, row.h}, {}};
        return {{x, row.y, 32, row.h}, {x + 32, row.y, valueW - 64, row.h}, {x + valueW - 32, row.y, 32, row.h}};
    }

    void drawSettings() {
        const bool overGame = settingsReturn_ == Screen::Paused && gba_;
        if (overGame) dimGame();
        else drawScene();
        const Rect panel = settingsPanel();
        drawPixelBox(panel, overGame ? palette::kPanelSolid : palette::kPanel, palette::kBorder, 3);
        drawMenuTitle(panel, "Settings");

        for (int i = 0; i < static_cast<int>(SettingRow::Count); ++i) {
            const auto row = static_cast<SettingRow>(i);
            const Rect r = settingsRowRect(i);
            const bool selected = i == settingsSelected_;
            if (row == SettingRow::Done) {
                const Rect button{r.x + (r.w - 160) / 2, r.y + 1, 160, r.h};
                if (selected) drawGoldButton(button, settingLabel(row), 15, false);
                else drawButton(button, settingLabel(row));
                continue;
            }
            if (selected) drawSelection(r);
            drawTextIn(r, settingLabel(row), 15, selected ? palette::kText : palette::kTextDim, selected, Align::Left,
                       24);
            const SettingsRowParts parts = settingsRowParts(i);
            drawTextIn(parts.value, fitText(settingValue(row), 15, true, parts.value.w), 15,
                       selected ? palette::kGoldLight : palette::kText, true,
                       hasArrows(row) ? Align::Center : Align::Right);
            if (hasArrows(row)) {
                const Color arrow = selected ? palette::kGold : withAlpha(palette::kTextDim, 0.7);
                drawTextIn(parts.left, "\u2039", 20, arrow, true, Align::Center);
                drawTextIn(parts.right, "\u203A", 20, arrow, true, Align::Center);
            }
        }

        const std::vector<Hint> hints =
            controller_ ? std::vector<Hint>{{"\u2190 \u2192", "Change"}, {confirmGlyph(), "Select"}, {backGlyph(), "Back"}}
                        : std::vector<Hint>{{"\u2190 \u2192", "Change"}, {"Return", "Select"}, {"Esc", "Back"}};
        drawHints(hints, panel.x + panel.w / 2, panel.y + panel.h - 30, panel.w - 24, Align::Center);
    }

    void drawGameList(const LibraryLayout& l) {
        const auto& games = library_.games();
        const int count = static_cast<int>(games.size());
        // Scroll so the selected row is always visible.
        if (selected_ < scrollTop_) scrollTop_ = selected_;
        if (selected_ >= scrollTop_ + l.visibleRows) scrollTop_ = selected_ - l.visibleRows + 1;
        scrollTop_ = std::clamp(scrollTop_, 0, std::max(0, count - l.visibleRows));

        constexpr float kThumbW = 54, kThumbH = 36;
        for (int i = scrollTop_; i < std::min(count, scrollTop_ + l.visibleRows); ++i) {
            const Rect row{l.list.x, l.list.y + (i - scrollTop_) * kRowHeight, l.list.w - 10, kRowHeight - 8};
            const bool selected = i == selected_;
            if (selected) drawSelection(row);
            else drawPixelBox(row, hovered(row) ? palette::kPanelHover : palette::kPanel);
            const RomEntry& game = games[i];
            const Rect thumb{row.x + 22, row.y + (row.h - kThumbH) / 2, kThumbW, kThumbH};
            drawThumbnail(thumb, game.path, 1);
            const float textX = thumb.x + kThumbW + 14, textW = row.x + row.w - 12 - textX;
            drawText(fitText(game.name, 15, true, textW), textX, row.y + 9, 15, palette::kText, true);
            std::string detail = game.title.empty() ? "No header title" : game.title;
            if (!game.code.empty()) detail += "  \u00B7  " + game.code;
            drawText(fitText(detail, 12, false, textW), textX, row.y + 31, 12,
                     selected ? palette::kGoldLight : palette::kTextDim);
        }

        if (count > l.visibleRows) {  // scrollbar
            const Rect track{l.list.x + l.list.w - 4, l.list.y, 4, l.visibleRows * kRowHeight - 8};
            fill(track, {255, 255, 255, 20});
            const float thumbH = std::max(24.0f, track.h * l.visibleRows / count);
            const float thumbY = track.y + (track.h - thumbH) * scrollTop_ / std::max(1, count - l.visibleRows);
            fill({track.x, thumbY, track.w, thumbH}, withAlpha(palette::kGold, 0.75));
        }
    }

    Rect playButton(const Rect& preview) const {
        return {preview.x + preview.w - 20 - 112, preview.y + preview.h - 20 - 38, 112, 38};
    }

    void drawPreview(const Rect& area, const RomEntry& game) {
        drawPixelBox(area, palette::kPanel, palette::kBorder, 3);
        constexpr float kPad = 20, kInfoH = 122, kBezel = 6;

        // Screenshot from the last time this game was played, in a dark bezel,
        // scaled by a whole number when possible so the pixel art stays sharp.
        const float maxW = area.w - 2 * (kPad + kBezel), maxH = area.h - 2 * (kPad + kBezel) - kInfoH;
        float w = maxW, h = w * kScreenHeight / kScreenWidth;
        if (h > maxH) { h = maxH; w = h * kScreenWidth / kScreenHeight; }
        const float whole = std::floor(w * scale_ / kScreenWidth);
        if (whole >= 1) { w = whole * kScreenWidth / scale_; h = whole * kScreenHeight / scale_; }
        const Rect shot{area.x + (area.w - w) / 2, area.y + kPad + kBezel + (maxH - h) / 2, w, h};
        if (w > 0 && h > 0) {
            drawPixelBox({shot.x - kBezel, shot.y - kBezel, shot.w + 2 * kBezel, shot.h + 2 * kBezel}, {8, 8, 22},
                         {255, 255, 255, 30});
            if (thumbnail(game.path)) {
                drawThumbnail(shot, game.path, 3);
            } else {
                fill(shot, {12, 12, 34});
                const float mw = artSize(moonW_, 3), mh = artSize(moonH_, 3);
                drawMoon(shot.x + (shot.w - mw) / 2, shot.y + shot.h / 2 - mh + 4, 3, 0.55);
                drawText("No screenshot yet", shot.x + shot.w / 2, shot.y + shot.h / 2 + 16, 13, palette::kTextDim,
                         false, Align::Center);
            }
        }

        const float textX = area.x + kPad, textW = area.w - kPad * 2;
        const float infoY = area.y + area.h - kPad - kInfoH;
        drawText(fitText(game.name, 20, true, textW), textX, infoY, 20, palette::kText, true);
        const std::string header = (game.title.empty() ? "Untitled" : game.title) +
                                   (game.code.empty() ? "" : "  \u00B7  " + game.code);
        drawText(fitText(header, 13, false, textW), textX, infoY + 32, 13, palette::kTextDim);
        drawText(fitText(std::filesystem::path(game.path).filename().string(), 12, false, textW), textX, infoY + 54,
                 12, withAlpha(palette::kTextDim, 0.75));

        const Rect play = playButton(area);
        const bool hasSave = fileExists(savePathFor(game.code, game.path));
        const std::string badge = hasSave ? "Saved game" : "No save yet";
        const Rect b{textX, play.y + (play.h - 26) / 2, textWidth(badge, 12, true) + 22, 26};
        if (hasSave) drawPixelBox(b, withAlpha(palette::kGold, 0.18), withAlpha(palette::kGold, 0.6));
        else drawPixelBox(b, {255, 255, 255, 14}, {255, 255, 255, 36});
        drawTextIn(b, badge, 12, hasSave ? palette::kGoldLight : palette::kTextDim, true, Align::Center);
        drawGoldButton(play, "\u25B6  Play", 15, hovered(play));
    }

public:
    // Developer tool (--ui-dump): draws each screen into BMP files in `dir`,
    // at a large and at the smallest window size, without showing a window.
    // With a ROM, the library shows that ROM's folder and the pause menu is
    // drawn over the game. Nothing is saved (no save file, settings or thumbnail).
    bool dumpUi(const std::string& dir, const std::string& romPath) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!romPath.empty()) {
            auto data = readFile(romPath);
            auto gba = std::make_unique<GBA>();
            std::string error;
            if (!data || !gba->loadRom(std::move(*data), &error)) {
                std::fprintf(stderr, "Could not load ROM '%s' %s\n", romPath.c_str(), error.c_str());
                return false;
            }
            for (int i = 0; i < 90; ++i) gba->runFrame();
            gba_ = std::move(gba);
            library_.setFolder(fs::absolute(romPath).parent_path().string());
            selected_ = std::max(0, library_.indexOf(fs::absolute(romPath).string()));
            // Use the current frame as the game's screenshot, without writing it to disk.
            if (selected_ < static_cast<int>(library_.games().size())) {
                const std::string path = library_.games()[selected_].path;
                forgetThumbnail(path);
                thumbnails_[path] = makeTexture(std::vector<u32>(gba_->ppu.framebuffer.begin(),
                                                                 gba_->ppu.framebuffer.end()),
                                                kScreenWidth, kScreenHeight, false);
            }
        }
        const std::string realFolder = library_.folder();
        bool ok = true;
        for (const auto& [w, h] : {std::pair{960, 640}, std::pair{640, 480}}) {
            SDL_SetWindowSize(window_, w, h);
            updateScale();
            int outW = 0, outH = 0;
            SDL_GetRendererOutputSize(renderer_, &outW, &outH);
            SDL_Texture* target =
                SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, outW, outH);
            if (!target) return false;
            SDL_SetRenderTarget(renderer_, target);
            const std::string size = std::to_string(w) + "x" + std::to_string(h);
            auto shot = [&](const std::string& name, auto draw) {
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
                SDL_RenderClear(renderer_);
                draw();
                std::vector<u32> pixels(static_cast<std::size_t>(outW) * outH);
                ok &= SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), outW * 4) == 0;
                for (u32& p : pixels) p |= 0xFF000000u;
                const std::string path = (fs::path(dir) / (name + "-" + size + ".bmp")).string();
                ok &= writeBmp(path, pixels.data(), outW, outH);
                std::printf("Wrote %s\n", path.c_str());
            };
            timeOverride_ = 0.95;
            screen_ = Screen::Library;
            shot("intro", [&] { drawLibrary(); drawIntro(); });
            timeOverride_ = 10;
            shot("library", [&] { drawLibrary(); });
            screen_ = Screen::Settings;
            settingsReturn_ = Screen::Library;
            settingsSelected_ = static_cast<int>(SettingRow::VideoFilter);
            shot("settings", [&] { drawSettings(); });
            if (gba_) {
                screen_ = Screen::Paused;
                pauseSelected_ = 0;
                shot("pause", [&] { drawPauseMenu(); });
                screen_ = Screen::Settings;
                settingsReturn_ = Screen::Paused;
                shot("settings-over-game", [&] { drawSettings(); });
            }
            screen_ = Screen::Library;
            library_.setFolder((fs::path(dir) / "empty-library").string());
            shot("empty-library", [&] { drawLibrary(); });
            library_.setFolder(realFolder);
            SDL_SetRenderTarget(renderer_, nullptr);
            SDL_DestroyTexture(target);
        }
        gba_.reset();  // without stopGame(): don't write a save or thumbnail
        return ok;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string romPath, dumpPath, savePath, uiDumpDir;
    int headlessFrames = 0, dumpEvery = 0, dumpFilter = -1;
    std::vector<std::pair<int, u16>> presses;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless" && i + 1 < argc) headlessFrames = std::stoi(argv[++i]);
        else if (arg == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (arg == "--dump-every" && i + 1 < argc) dumpEvery = std::stoi(argv[++i]);
        else if (arg == "--save" && i + 1 < argc) savePath = argv[++i];
        else if (arg == "--filter" && i + 1 < argc) dumpFilter = std::stoi(argv[++i]);  // 0-6, see VideoFilter
        else if (arg == "--ui-dump" && i + 1 < argc) uiDumpDir = argv[++i];
        else if (arg == "--press" && i + 1 < argc) {
            // Format: frame:key[,frame:key...]  e.g. 300:start,400:a
            std::stringstream list(argv[++i]);
            std::string item;
            while (std::getline(list, item, ',')) {
                const auto colon = item.find(':');
                if (colon == std::string::npos) continue;
                presses.emplace_back(std::stoi(item.substr(0, colon)), keyFromName(item.substr(colon + 1)));
            }
        } else if (arg.rfind("-psn", 0) == 0) {
            // macOS may pass a process serial number when launched from Finder.
        } else romPath = arg;
    }

    if (SDL_Init(headlessFrames > 0 ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int result = 0;
    if (headlessFrames > 0) {
        auto gba = std::make_unique<GBA>();
        auto data = readFile(romPath);
        std::string error;
        if (!data || !gba->loadRom(std::move(*data), &error)) {
            std::fprintf(stderr, "Could not load ROM '%s' %s\n", romPath.c_str(), error.c_str());
            result = 1;
        } else {
            std::printf("Loaded \"%s\" (%s)\n", gba->gameTitle().c_str(), gba->gameCode().c_str());
            if (!savePath.empty()) {
                std::printf("Save file %s\n", gba->loadSave(savePath) ? "loaded" : "not found (new save)");
            }
            result = runHeadless(*gba, headlessFrames, dumpPath, presses, dumpEvery,
                                 std::clamp(dumpFilter, -1, kVideoFilterCount - 1));
            if (!savePath.empty() && gba->writeSave(savePath)) std::printf("Save written to %s\n", savePath.c_str());
        }
    } else if (!uiDumpDir.empty()) {
        App app;
        if (!app.init(true) || !app.dumpUi(uiDumpDir, romPath)) {
            std::fprintf(stderr, "UI dump failed: %s\n", SDL_GetError());
            result = 1;
        }
    } else {
        copyDataFromOldName();
        App app;
        if (!app.init()) {
            std::fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
            result = 1;
        } else {
            if (!romPath.empty()) app.startGame(romPath);  // otherwise start on the library
            app.run();
        }
    }
    SDL_Quit();
    return result;
}
