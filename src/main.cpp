// main.cpp - LaLune Dorée Emulator's macOS frontend built on SDL2 (window, input, audio, launcher UI).
//
// The app opens on a game library. Pick a game to play it; open the pause
// menu (Esc, or the PS / mic button) to reset, go back to the library or quit.
// Settings (volume, video, controller layout, games folder...) open from the
// library (",", Create button) or the pause menu, and are saved to settings.ini.
// An empty games folder shows an "Insert a game" prompt instead of a list.
// Only the PS5 DualSense is supported as a controller (Xbox pads are not).
//
// Default controls (all changeable on Settings > Controls; see controls.cpp):
//   Keyboard: arrow keys = D-pad    X = A    Z = B    A = L    S = R
//             Return = Start    Backspace / Right Shift = Select
//             Tab (hold) = fast-forward    ` = slow motion on/off (half speed)
//   DualSense: left stick / D-pad = D-pad    Cross = A    Square / Circle = B
//             L1 / L2 = L    R1 / R2 = R    Options = Start    Create = Select
//             touchpad click (hold) = fast-forward    R3 = slow motion on/off
//   Turbo A / Turbo B are unassigned until you pick keys for them.
//   Save states: F5 or Cmd+S (L3 on the controller) = quick save
//                F8 or Cmd+L = quick load (the newest state you saved)
//   The pause menu's "Save states" screen lists every state to load or delete.
// Fixed keys: Esc / PS / mic button = pause menu    F / F11 = toggle fullscreen
//   V / Shift+V = next / previous video filter    3-6 = window size 3x-6x
//   (F, V and 3-6 go to the game instead if you assign them to a control.)
//
// Usage:
//   LaLuneDoree [rom.gba]                    open the library, or play a ROM directly
//   LaLuneDoree rom.gba --headless 600 --dump out.bmp [--dump-every N] [--press 300:start,...] [--save file.sav]
//                             [--filter N]  (screenshot through video filter N, 0-6; see video_filters.hpp)
//                             [--load-state in.state] [--save-state out.state]  (before / after the run)
//   LaLuneDoree [rom.gba] --ui-dump folder   draw every menu screen into BMP files (for checking the UI)
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include "save_states.hpp"
#include "video_filters.hpp"
#include "settings.hpp"

namespace {

constexpr const char* kAppName = "LaLune Dor\u00E9e Emulator";

// Reads a ROM file, stopping just past the 32 MiB a GBA ROM can be at most.
// Anything longer is cut off there (and then rejected by GBA::loadRom), so a
// huge or endless file can't use up all the Mac's memory.
std::optional<std::vector<u8>> readFile(const std::string& path) {
    constexpr std::size_t kMaxRomSize = 32 * 1024 * 1024;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::vector<u8> data;
    char buffer[64 * 1024];
    while (data.size() <= kMaxRomSize && (file.read(buffer, sizeof buffer) || file.gcount() > 0)) {
        data.insert(data.end(), buffer, buffer + file.gcount());
    }
    return data;
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
std::string saveNameFor(const std::string& gameCode, const std::string& romPath) {
    const bool validCode = gameCode.size() == 4 &&
                           std::all_of(gameCode.begin(), gameCode.end(),
                                       [](unsigned char c) { return std::isalnum(c); });
    return validCode ? gameCode : std::filesystem::path(romPath).stem().string();
}

std::string savePathFor(const std::string& gameCode, const std::string& romPath) {
    return prefPath() + saveNameFor(gameCode, romPath) + ".sav";
}

// Save states go in a folder per game, named like its save file (States/BLRE/).
std::string statesFolderFor(const std::string& gameCode, const std::string& romPath) {
    return prefPath() + "States/" + saveNameFor(gameCode, romPath);
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
enum class MenuAction { Up, Down, Left, Right, Accept, Back, AddRoms, OpenFolder, OpenSettings, Clear };

enum class Screen { Library, Playing, Paused, Settings, Controls, States };

// Pause menu items, in display order.
enum class PauseItem { Resume, SaveStates, Reset, Settings, Fullscreen, Library, Quit, Count };

// Rows on the settings screen, in display order.
enum class SettingRow {
    Volume, FastForward, VideoFilter, Scaling, Fullscreen, Controls, ConfirmButton, RomFolder, Done, Count
};

// Rows below the button list on the Controls screen.
enum class ControlOption { StickDpad, StickDeadzone, TriggerPoint, TurboSpeed, FastForwardMode, Count };
constexpr int kControlOptionCount = static_cast<int>(ControlOption::Count);

constexpr u32 controlBit(Control c) { return 1u << static_cast<int>(c); }
// Controls that press GBA buttons (as opposed to fast-forward and slow motion).
constexpr u32 kGameControls = (1u << static_cast<int>(Control::TurboB)) * 2 - 1;

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

// The games folder to open at launch. A folder picked in Settings has a
// bookmark: the sandboxed app needs it to be allowed back into that folder,
// and it also follows the folder if it was moved or renamed.
std::string startupRomFolder(Settings& settings) {
    if (settings.romFolder.empty()) return defaultRomFolder();
    if (!settings.romFolderBookmark.empty()) {
        if (std::string path = macui::openFolderBookmark(settings.romFolderBookmark); !path.empty()) {
            settings.romFolder = path;
        }
    }
    return settings.romFolder;
}

class App {
public:
    App()
        : settings_(Settings::load(settingsPath())),
          library_(startupRomFolder(settings_), prefPath() + "Thumbnails") {}

    ~App() {
        clearTextCache();
        clearThumbnails();
        clearStateScreenshots();
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
        macui::keepControllerButtonsFromSystem();  // the PS button opens our menu, not the Games overlay
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
        states_.open(statesFolderFor(gba_->gameCode(), path), gba_->romHash());
        SDL_SetWindowTitle(window_, (std::string(kAppName) + " - " + gameName(path)).c_str());
        screen_ = Screen::Playing;
        suppressInput_ = true;
        tapped_ = 0;
        slowMotion_ = false;  // each game starts at normal speed
        fastForwardOn_ = false;
        prevControls_ = heldControls();
        lastSample_[0] = lastSample_[1] = 0;
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
            case Screen::Controls:
                drawControls();
                SDL_RenderPresent(renderer_);
                SDL_Delay(16);
                break;
            case Screen::States:
                drawStates();
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
    // Slow motion makes every frame last this many times longer (2 = half speed).
    static constexpr int kSlowMotionFactor = 2;
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

    // Save States screen (opened from the pause menu). Row 0 is "Save a new
    // state"; row i > 0 is states_.list()[i - 1].
    SaveStates states_;
    int statesSelected_ = 0;
    int statesScroll_ = 0;       // first row shown
    int statesDeleteArmed_ = -1; // row waiting for a second Delete press, or -1
    std::unordered_map<std::string, SDL_Texture*> stateScreenshots_;  // by path; "" = the game right now
    Clock::time_point nextFrame_ = Clock::now(), lastSave_ = Clock::now();
    bool fastForwarding_ = false;
    bool slowMotion_ = false;   // toggled with ` or R3; holding fast-forward overrides it
    s16 lastSample_[2] = {};    // last stereo sample queued, for smooth slow-motion audio
    std::vector<s16> stretched_;  // slow-motion audio buffer, kept to avoid reallocating

    // Input. "Controls" are bits of controlBit(Control::...), see controls.hpp.
    std::vector<int> heldKeys_;   // keyboard keys held down that are assigned to a control
    u32 padControls_ = 0;     // controls held on the controller this frame
    u32 padDirections_ = 0;   // the controller's own D-pad and left stick, for menus (Key bits)
    u32 tapped_ = 0;          // controls pressed since the last frame (even if already released)
    u32 prevControls_ = 0;    // controls held last frame, to spot new presses
    bool fastForwardOn_ = false;  // "Toggle" fast-forward mode: switched on
    int turboTick_ = 0;       // frames since a turbo button was pressed
    bool suppressInput_ = false;  // ignore buttons still held from a menu until released
    bool stickActive_ = false;
    int stickSector_ = -1;    // last 8-way direction of the left stick, -1 = centred

    // Controls screen
    int controlsRow_ = 0, controlsCol_ = 0;  // selection; binding rows have 4 columns (2 keys, 2 buttons)
    int controlsScroll_ = 0;                 // first row shown
    bool capturing_ = false;                 // waiting for the key or button to assign
    Uint32 captureUntil_ = 0;                // capture gives up at this time (ms)
    bool captureTriggerDown_[2] = {};        // L2/R2 already held when capture started
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

    // Which keys and controller buttons press what is in settings_.bindings
    // (changed on the Controls screen; defaults in controls.cpp).
    // Extra angle (radians, ~7 degrees) the stick must move past a sector
    // boundary before the direction changes. Stops jitter between straight
    // and diagonal movement.
    static constexpr double kSectorHysteresis = 0.12;

    // -----------------------------------------------------------------------
    // Running a game
    // -----------------------------------------------------------------------

    void stepGame() {
        const u32 held = heldControls();
        // New presses: from key and button events (so a quick tap between
        // two frames still counts), and L2/R2, which are polled.
        const u32 pressed = tapped_ | (held & ~prevControls_);
        prevControls_ = held;
        tapped_ = 0;

        if (pressed & controlBit(Control::SlowMotion)) toggleSlowMotion();
        // States are saved and loaded between frames, before this one runs.
        if (pressed & controlBit(Control::QuickSave)) quickSave();
        else if (pressed & controlBit(Control::QuickLoad)) quickLoad();
        if (settings_.fastForwardToggle) {
            if (pressed & controlBit(Control::FastForward)) fastForwardOn_ = !fastForwardOn_;
            fastForwarding_ = fastForwardOn_;
        } else {
            fastForwarding_ = held & controlBit(Control::FastForward);
        }

        u32 game = (held | pressed) & kGameControls;
        if (suppressInput_) {
            // E.g. Cross was pressed to pick the game: don't also send A to it.
            if (game == 0) suppressInput_ = false;
            game = 0;
        }
        // Turbo buttons press for turboFrames frames, then release for as many.
        // A new press starts on "pressed", so even a short tap registers.
        if (pressed & (controlBit(Control::TurboA) | controlBit(Control::TurboB))) turboTick_ = 0;
        const bool turboDown = (turboTick_++ / std::max(1, settings_.turboFrames)) % 2 == 0;
        gba_->setKeys(cleanDirections(gbaKeys(game, turboDown)));
        const int framesToRun = fastForwarding_ ? settings_.fastForwardSpeed : 1;
        for (int i = 0; i < framesToRun; ++i) gba_->runFrame();
        queueAudio();
        frameDirty_ = true;
        drawGameFrame();
        if (fastForwarding_) drawFastForwardBadge();
        else if (slowMotion_) drawSlowMotionBadge();
        drawToast();
        SDL_RenderPresent(renderer_);

        // Pace the emulator. With audio we wait until the queue drains;
        // that keeps sound smooth. Without audio we use the wall clock.
        // In slow motion queueAudio stretches each frame's sound, so the
        // same wait slows the game down too.
        if (!fastForwarding_) {
            if (audio_) {
                while (SDL_GetQueuedAudioSize(audio_) > kTargetAudioQueue) SDL_Delay(1);
            } else {
                const double frameTime = (slowMotion_ ? kSlowMotionFactor : 1) / 59.7275;
                nextFrame_ += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frameTime));
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
        tapped_ = 0;
        prevControls_ = heldControls();  // a button held from the menu isn't a new press
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
            if (slowMotion_ && !fastForwarding_) {
                // Play the sound slower (and lower, like a slowed-down tape):
                // fill in extra samples on a straight line between each pair.
                stretched_.clear();
                stretched_.reserve(samples.size() * kSlowMotionFactor);
                for (std::size_t i = 0; i + 1 < samples.size(); i += 2) {
                    for (int step = 1; step <= kSlowMotionFactor; ++step)
                        for (int ch = 0; ch < 2; ++ch)
                            stretched_.push_back(static_cast<s16>(
                                lastSample_[ch] + (samples[i + ch] - lastSample_[ch]) * step / kSlowMotionFactor));
                    lastSample_[0] = samples[i];
                    lastSample_[1] = samples[i + 1];
                }
                SDL_QueueAudio(audio_, stretched_.data(), static_cast<Uint32>(stretched_.size() * sizeof(s16)));
            } else if (!fastForwarding_ || SDL_GetQueuedAudioSize(audio_) < kTargetAudioQueue) {
                // While fast-forwarding, drop audio instead of building a backlog.
                SDL_QueueAudio(audio_, samples.data(), static_cast<Uint32>(samples.size() * sizeof(s16)));
            }
            if (samples.size() >= 2) {
                lastSample_[0] = samples[samples.size() - 2];
                lastSample_[1] = samples[samples.size() - 1];
            }
        }
        samples.clear();
    }

    void toggleSlowMotion() {
        slowMotion_ = !slowMotion_;
        showToast(slowMotion_ ? "Slow motion on" : "Slow motion off");
    }

    // -----------------------------------------------------------------------
    // Save states (see save_states.hpp)
    // -----------------------------------------------------------------------

    // Saves the game as it is right now. Says why if it couldn't.
    bool saveStateNow() {
        std::string error;
        if (states_.save(*gba_, StateKind::Saved, error)) return true;
        showToast(error);
        return false;
    }

    void quickSave() {
        if (saveStateNow()) showToast("State saved");
    }

    // Loads the newest state you saved (backups are skipped, so pressing it
    // twice doesn't flip between two moments).
    void quickLoad() {
        states_.refresh();
        const StateInfo* latest = states_.latest(StateKind::Saved);
        if (!latest) {
            showToast("No saved state yet");
            return;
        }
        loadState(latest->path);
    }

    // Loads a state. The moment you were at is kept first, as a backup state,
    // so going back is never a one-way trip. `path` is a copy on purpose:
    // saving the backup rebuilds the list it may have come from.
    bool loadState(const std::string path) {
        std::string error;
        const auto file = SaveStates::readFile(path, *gba_, error);
        if (!file) {
            showToast(error);
            return false;
        }
        if (!states_.save(*gba_, StateKind::Backup, error)) {
            showToast("Not loaded: couldn't keep a backup of where you are");
            return false;
        }
        if (!SaveStates::apply(*gba_, *file)) {
            showToast("This state file is damaged.");
            return false;
        }
        if (audio_) SDL_ClearQueuedAudio(audio_);
        lastSample_[0] = lastSample_[1] = 0;
        frameDirty_ = true;
        showToast("Loaded state from " + stateTimeLabel(file->info.createdMs));
        return true;
    }

    // When a state was made, e.g. "Today, 3:42:05 PM", "Tuesday, 9:10:00 AM"
    // or "Oct 3, 2026, 8:01:12 PM".
    static std::string stateTimeLabel(s64 createdMs) {
        const std::time_t made = static_cast<std::time_t>(createdMs / 1000), current = std::time(nullptr);
        std::tm when{}, now{};
        localtime_r(&made, &when);
        localtime_r(&current, &now);
        char clock[32];
        std::snprintf(clock, sizeof clock, "%d:%02d:%02d %s", when.tm_hour % 12 == 0 ? 12 : when.tm_hour % 12,
                      when.tm_min, when.tm_sec, when.tm_hour < 12 ? "AM" : "PM");
        // Calendar days between the two dates (compared at noon, so daylight
        // saving changes don't matter).
        auto noon = [](std::tm t) {
            t.tm_hour = 12;
            t.tm_min = t.tm_sec = 0;
            t.tm_isdst = -1;
            return std::mktime(&t);
        };
        const long days = std::lround(std::difftime(noon(now), noon(when)) / 86400);
        std::string date;
        if (days == 0) date = "Today";
        else if (days == 1) date = "Yesterday";
        else {
            char text[32];
            if (days > 1 && days < 7) std::strftime(text, sizeof text, "%A", &when);
            else std::snprintf(text, sizeof text, "%s %d, %d", kMonths[when.tm_mon % 12], when.tm_mday,
                               when.tm_year + 1900);
            date = text;
        }
        return date + ", " + clock;
    }

    static constexpr const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

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

    // In menus, "Circle confirms" swaps Cross and Circle. (In games the
    // Controls screen decides what each button does.)
    Uint8 physicalToLogical(Uint8 button) const {
        if (!settings_.circleIsA) return button;
        if (button == SDL_CONTROLLER_BUTTON_A) return SDL_CONTROLLER_BUTTON_B;
        if (button == SDL_CONTROLLER_BUTTON_B) return SDL_CONTROLLER_BUTTON_A;
        return button;
    }

    // Button glyphs for on-screen hints, following the confirm-button setting.
    const char* confirmGlyph() const { return settings_.circleIsA ? "\u25CB" : "\u2715"; }
    const char* backGlyph() const { return settings_.circleIsA ? "\u2715" : "\u25CB"; }

    // The controls a keyboard key or controller input is assigned to.
    u32 controlsForKey(int key) const {
        u32 controls = 0;
        for (int c = 0; c < kControlCount; ++c)
            for (int slot : settings_.bindings.keys[c])
                if (slot == key) controls |= 1u << c;
        return controls;
    }

    u32 controlsForPad(int input) const {
        u32 controls = 0;
        for (int c = 0; c < kControlCount; ++c)
            for (int slot : settings_.bindings.pad[c])
                if (slot == input) controls |= 1u << c;
        return controls;
    }

    // Everything held right now, on the keyboard and the controller.
    u32 heldControls() const {
        u32 controls = padControls_;
        for (int key : heldKeys_) controls |= controlsForKey(key);
        return controls;
    }

    // The GBA buttons pressed by `controls`. Turbo buttons only press while
    // `turboDown` (they flicker on and off).
    static u16 gbaKeys(u32 controls, bool turboDown) {
        static constexpr std::pair<Control, u16> kKeys[] = {
            {Control::Up, KeyUp}, {Control::Down, KeyDown}, {Control::Left, KeyLeft}, {Control::Right, KeyRight},
            {Control::A, KeyA}, {Control::B, KeyB}, {Control::L, KeyL}, {Control::R, KeyR},
            {Control::Start, KeyStart}, {Control::Select, KeySelect},
        };
        u16 keys = 0;
        for (const auto& [control, key] : kKeys)
            if (controls & controlBit(control)) keys |= key;
        if (turboDown && (controls & controlBit(Control::TurboA))) keys |= KeyA;
        if (turboDown && (controls & controlBit(Control::TurboB))) keys |= KeyB;
        return keys;
    }

    // How far L2 / R2 must be pressed to count, on SDL's 0-32767 scale.
    int triggerThreshold() const { return settings_.triggerPoint * 32767 / 100; }

    bool triggerDown(int side) const {
        const auto axis = side == 0 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        return controller_ && SDL_GameControllerGetAxis(controller_, axis) > triggerThreshold();
    }

    bool padInputDown(int input) const {
        if (!controller_ || input == kUnbound) return false;
        if (input == kPadTriggerLeft) return triggerDown(0);
        if (input == kPadTriggerRight) return triggerDown(1);
        if (input < 0 || input >= SDL_CONTROLLER_BUTTON_MAX) return false;
        return SDL_GameControllerGetButton(controller_, static_cast<SDL_GameControllerButton>(input));
    }

    void updateController() {
        padControls_ = 0;
        padDirections_ = 0;
        if (!controller_) return;
        for (int c = 0; c < kControlCount; ++c)
            for (int input : settings_.bindings.pad[c])
                if (padInputDown(input)) padControls_ |= 1u << c;
        const u32 stick = stickDirection();
        if (settings_.leftStickDpad) {
            if (stick & KeyUp) padControls_ |= controlBit(Control::Up);
            if (stick & KeyDown) padControls_ |= controlBit(Control::Down);
            if (stick & KeyLeft) padControls_ |= controlBit(Control::Left);
            if (stick & KeyRight) padControls_ |= controlBit(Control::Right);
        }
        // Menus always follow the controller's own D-pad and left stick,
        // whatever the game buttons are set to, so you can't get stuck.
        padDirections_ = stick;
        static constexpr std::pair<SDL_GameControllerButton, u32> kDpad[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, KeyUp}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, KeyDown},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, KeyLeft}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, KeyRight},
        };
        for (const auto& [button, key] : kDpad)
            if (SDL_GameControllerGetButton(controller_, button)) padDirections_ |= key;
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
        // Radial dead zone with hysteresis: the stick must pass the dead zone
        // to start moving and drop 10% below it to stop, so it doesn't flicker.
        const double press = settings_.stickDeadzone / 100.0, release = std::max(0.05, press - 0.1);
        stickActive_ = std::hypot(x, y) > (stickActive_ ? release : press);
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
        if (padDirections_ & KeyUp) direction = 1;
        else if (padDirections_ & KeyDown) direction = 2;
        else if (padDirections_ & KeyLeft) direction = 3;
        else if (padDirections_ & KeyRight) direction = 4;
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
            case SDL_KEYUP: std::erase(heldKeys_, static_cast<int>(event.key.keysym.sym)); break;
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
                macui::keepControllerButtonsFromSystem();
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
        if (capturing_) {
            updateCapture();
            return;  // menus wait until the new key or button is picked
        }
        if (screen_ != Screen::Playing) updateMenuRepeat(actions);
        if (introActive()) actions.clear();
        for (MenuAction action : actions) {
            if (screen_ == Screen::Library) libraryAction(action);
            else if (screen_ == Screen::Paused) pauseAction(action);
            else if (screen_ == Screen::Settings) settingsAction(action);
            else if (screen_ == Screen::Controls) controlsAction(action);
            else if (screen_ == Screen::States) statesAction(action);
        }
    }

    void onKeyDown(const SDL_KeyboardEvent& key, std::vector<MenuAction>& actions) {
        const SDL_Keycode sym = key.keysym.sym;
        if (capturing_) {
            if (!key.repeat) captureKey(key);
            return;
        }
        if (screen_ == Screen::Playing) {
            if (sym == SDLK_ESCAPE) {
                if (!key.repeat) pauseGame();
                return;
            }
            // A key assigned on the Controls screen goes to the game, even if
            // it's also a shortcut below. (Cmd shortcuts like Cmd+Q never do.)
            const u32 controls = (key.keysym.mod & KMOD_GUI) ? 0 : controlsForKey(sym);
            if (controls) {
                if (std::find(heldKeys_.begin(), heldKeys_.end(), sym) == heldKeys_.end()) heldKeys_.push_back(sym);
                if (!key.repeat) tapped_ |= controls;
                return;
            }
            if (key.repeat) return;
            if ((key.keysym.mod & KMOD_GUI) && sym == SDLK_s) quickSave();       // like Save in Mac apps
            else if ((key.keysym.mod & KMOD_GUI) && sym == SDLK_l) quickLoad();
            else if (sym == SDLK_f || sym == SDLK_F11) toggleFullscreen();
            else if (sym == SDLK_v) {  // cycle video filters (Shift+V goes backwards)
                setVideoFilter(settings_.videoFilter + ((key.keysym.mod & KMOD_SHIFT) ? -1 : 1));
                showToast(std::string("Filter: ") + videoFilterName(currentFilter()));
            } else if (sym >= SDLK_3 && sym <= SDLK_6) {  // smaller sizes are below the minimum window size
                setWindowScale(3 + (sym - SDLK_3));
            }
            return;
        }
        if (sym == SDLK_f || sym == SDLK_F11) {
            if (!key.repeat) toggleFullscreen();
            return;
        }
        // Menus. Arrows may auto-repeat; the rest only fire once per press.
        if (sym == SDLK_UP) actions.push_back(MenuAction::Up);
        else if (sym == SDLK_DOWN) actions.push_back(MenuAction::Down);
        else if (sym == SDLK_LEFT) actions.push_back(MenuAction::Left);
        else if (sym == SDLK_RIGHT) actions.push_back(MenuAction::Right);
        if (key.repeat) return;
        // Lists where Delete removes the selected entry instead of going back.
        const bool listScreen = screen_ == Screen::Controls || screen_ == Screen::States;
        switch (sym) {
        case SDLK_COMMA: actions.push_back(MenuAction::OpenSettings); break;  // also Cmd+, like other Mac apps
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: actions.push_back(MenuAction::Accept); break;
        case SDLK_BACKSPACE: case SDLK_DELETE:
            actions.push_back(listScreen ? MenuAction::Clear : MenuAction::Back);
            break;
        case SDLK_ESCAPE: case SDLK_z: actions.push_back(MenuAction::Back); break;
        case SDLK_o: actions.push_back(MenuAction::AddRoms); break;
        case SDLK_g: actions.push_back(MenuAction::OpenFolder); break;
        default: break;
        }
    }

    void onButtonDown(Uint8 button, std::vector<MenuAction>& actions) {
        if (capturing_) {
            capturePadButton(button);
            return;
        }
        const bool menuButton = isReservedPadInput(button);  // PS or mic
        if (screen_ == Screen::Playing) {
            if (menuButton) pauseGame();
            else tapped_ |= controlsForPad(button);
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
        case SDL_CONTROLLER_BUTTON_Y:                                                      // Triangle
            actions.push_back(screen_ == Screen::Controls || screen_ == Screen::States ? MenuAction::Clear
                                                                                        : MenuAction::AddRoms);
            break;
        case SDL_CONTROLLER_BUTTON_X: actions.push_back(MenuAction::OpenFolder); break;    // Square
        case SDL_CONTROLLER_BUTTON_START:
            actions.push_back(screen_ == Screen::Library ? MenuAction::Accept : MenuAction::Back);
            break;
        default: break;  // directions come from updateMenuRepeat
        }
    }

    void onMouseMove(float x, float y) {
        mouseX_ = x;
        mouseY_ = y;
        if (screen_ == Screen::Controls) {
            if (!capturing_) controlsHitTest(x, y, controlsRow_, controlsCol_);
            return;
        }
        if (screen_ == Screen::Settings) {
            for (int i = 0; i < static_cast<int>(SettingRow::Count); ++i)
                if (settingsRowRect(i).contains(x, y)) settingsSelected_ = i;
            return;
        }
        if (screen_ == Screen::States) {
            if (const int row = statesHitTest(x, y); row >= 0 && row != statesSelected_) {
                statesSelected_ = row;
                statesDeleteArmed_ = -1;
            }
            return;
        }
        if (screen_ != Screen::Paused) return;
        const auto items = pauseItems();
        for (std::size_t i = 0; i < items.size(); ++i)
            if (pauseItemRect(static_cast<int>(i)).contains(x, y)) pauseSelected_ = static_cast<int>(i);
    }

    void onClick(float x, float y, int clicks) {
        if (capturing_) {  // clicking anywhere cancels picking a key
            stopCapture("");
            return;
        }
        if (screen_ == Screen::Controls) {
            int row = 0, col = 0;
            if (!controlsHitTest(x, y, row, col)) return;
            controlsRow_ = row;
            controlsCol_ = col;
            // On option rows the ‹ and › arrows step the value; elsewhere a click acts like Return.
            if (row >= kControlCount && row < kControlCount + kControlOptionCount) {
                const Rect r = controlsRowRect(row);
                const Rect value = controlsValueArea(r);
                if (x < value.x + 32) return controlsAction(MenuAction::Left);
                if (x >= value.x + value.w - 32) return controlsAction(MenuAction::Right);
            }
            controlsAction(MenuAction::Accept);
            return;
        }
        if (screen_ == Screen::States) {
            // A click picks a row and a double-click (or the buttons) acts on it,
            // like the library, so a stray click can't load a state.
            const StatesLayout l = statesLayout();
            if (l.mainButton.contains(x, y)) return statesAction(MenuAction::Accept);
            if (stateAt(statesSelected_) && l.deleteButton.contains(x, y)) return statesAction(MenuAction::Clear);
            if (const int row = statesHitTest(x, y); row >= 0) {
                if (row != statesSelected_) statesDeleteArmed_ = -1;
                statesSelected_ = row;
                if (clicks >= 2) statesAction(MenuAction::Accept);
            }
            return;
        }
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
        if (screen_ != Screen::Library && screen_ != Screen::Controls && screen_ != Screen::States) return;
        float amount = wheel.preciseY;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) amount = -amount;
        wheelAccum_ += amount;
        if (screen_ == Screen::Controls) {  // scroll the list without moving the selection
            const int maxScroll = std::max(0, controlsRowCount() - controlsVisibleRows());
            while (wheelAccum_ >= 1) { controlsScroll_ = std::max(0, controlsScroll_ - 1); wheelAccum_ -= 1; }
            while (wheelAccum_ <= -1) { controlsScroll_ = std::min(maxScroll, controlsScroll_ + 1); wheelAccum_ += 1; }
            return;
        }
        if (screen_ == Screen::States) {  // moves the selection (without wrapping around)
            while (wheelAccum_ >= 1) { if (statesSelected_ > 0) statesAction(MenuAction::Up); wheelAccum_ -= 1; }
            while (wheelAccum_ <= -1) {
                if (statesSelected_ < statesRowCount() - 1) statesAction(MenuAction::Down);
                wheelAccum_ += 1;
            }
            return;
        }
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
        heldKeys_.clear();  // key-up events may have gone to the dialog
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

    // Labels in PauseItem order.
    std::vector<std::string> pauseItems() const {
        const bool full = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        return {"Resume", "Save states\u2026", "Reset game", "Settings", full ? "Exit full screen" : "Full screen",
                "Back to library", "Quit"};
    }

    void pauseAction(MenuAction action) {
        const int count = static_cast<int>(PauseItem::Count);
        switch (action) {
        case MenuAction::Up: pauseSelected_ = (pauseSelected_ + count - 1) % count; break;
        case MenuAction::Down: pauseSelected_ = (pauseSelected_ + 1) % count; break;
        case MenuAction::Back: resumeGame(); break;
        case MenuAction::Accept:
            switch (static_cast<PauseItem>(pauseSelected_)) {
            case PauseItem::Resume: resumeGame(); break;
            case PauseItem::SaveStates: openStates(); break;
            case PauseItem::Reset:
                gba_->writeSave(savePath_);
                gba_->reset();
                resumeGame();
                break;
            case PauseItem::Settings: openSettings(); break;
            case PauseItem::Fullscreen: toggleFullscreen(); break;
            case PauseItem::Library: backToLibrary(); break;
            case PauseItem::Quit: running_ = false; break;
            case PauseItem::Count: break;
            }
            break;
        default: break;
        }
    }

    // ---- Save States screen ----

    int statesRowCount() const { return 1 + static_cast<int>(states_.list().size()); }

    // The state on a row, or nullptr for row 0 ("Save a new state").
    const StateInfo* stateAt(int row) const {
        return row >= 1 && row < statesRowCount() ? &states_.list()[row - 1] : nullptr;
    }

    void openStates() {
        states_.refresh();
        screen_ = Screen::States;
        statesSelected_ = statesScroll_ = 0;
        statesDeleteArmed_ = -1;
    }

    void closeStates() {
        clearStateScreenshots();
        screen_ = Screen::Paused;
        pauseSelected_ = static_cast<int>(PauseItem::SaveStates);
    }

    void statesAction(MenuAction action) {
        const int count = statesRowCount();
        const int before = statesSelected_;
        switch (action) {
        case MenuAction::Up: statesSelected_ = (statesSelected_ + count - 1) % count; break;
        case MenuAction::Down: statesSelected_ = (statesSelected_ + 1) % count; break;
        case MenuAction::Accept:
            if (const StateInfo* state = stateAt(statesSelected_)) {
                if (loadState(state->path)) {
                    clearStateScreenshots();
                    resumeGame();
                    return;
                }
                states_.refresh();  // e.g. the file was deleted in Finder
            } else if (saveStateNow()) {
                forgetStateScreenshot("");
                statesSelected_ = 1;  // the new state is the newest, at the top
                showToast("State saved");
            }
            break;
        case MenuAction::Clear:
            if (const StateInfo* state = stateAt(statesSelected_)) {
                // Deleting takes two presses, so one slip can't lose a state.
                if (statesDeleteArmed_ != statesSelected_) {
                    statesDeleteArmed_ = statesSelected_;
                    showToast(std::string("Press ") + (controller_ ? "\u25B3" : "\u232B") + " again to delete");
                    return;
                }
                const std::string path = state->path;
                forgetStateScreenshot(path);
                if (states_.remove(path)) showToast("State deleted");
                else showToast("Couldn't delete the state");
            }
            break;
        case MenuAction::Back: case MenuAction::OpenSettings: closeStates(); return;
        default: break;
        }
        statesSelected_ = std::clamp(statesSelected_, 0, statesRowCount() - 1);
        if (statesSelected_ != before || action == MenuAction::Clear) statesDeleteArmed_ = -1;
    }

    void clearStateScreenshots() {
        for (auto& [path, texture] : stateScreenshots_)
            if (texture) SDL_DestroyTexture(texture);
        stateScreenshots_.clear();
    }

    void forgetStateScreenshot(const std::string& path) {
        if (auto it = stateScreenshots_.find(path); it != stateScreenshots_.end()) {
            if (it->second) SDL_DestroyTexture(it->second);
            stateScreenshots_.erase(it);
        }
    }

    // A state's screenshot, read from its file once and then kept until the
    // screen closes. `path` "" is the game as it is now.
    SDL_Texture* stateScreenshot(const std::string& path) {
        if (auto it = stateScreenshots_.find(path); it != stateScreenshots_.end()) return it->second;
        SDL_Texture* texture = nullptr;
        if (path.empty()) {
            if (gba_) texture = makeTexture(std::vector<u32>(gba_->ppu.framebuffer.begin(), gba_->ppu.framebuffer.end()),
                                            kScreenWidth, kScreenHeight, false);
        } else if (std::vector<u32> pixels; SaveStates::readScreenshot(path, pixels)) {
            texture = makeTexture(pixels, kScreenWidth, kScreenHeight, false);
        }
        if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);  // GBA pixels have no real alpha
        stateScreenshots_[path] = texture;
        return texture;
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
        if (screen_ == Screen::Paused) pauseSelected_ = static_cast<int>(PauseItem::Settings);  // keep it highlighted
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
        case SettingRow::Controls:
            if (step == 0) openControls();
            return;
        case SettingRow::ConfirmButton: {
            // If A and B are still on their default buttons, move them with
            // the confirm button, so Circle confirms and is A (and back).
            const Bindings before = Bindings::defaults(settings_.circleIsA);
            settings_.circleIsA = !settings_.circleIsA;
            const Bindings after = Bindings::defaults(settings_.circleIsA);
            auto& pad = settings_.bindings.pad;
            const int a = static_cast<int>(Control::A), b = static_cast<int>(Control::B);
            if (pad[a] == before.pad[a] && pad[b] == before.pad[b]) {
                pad[a] = after.pad[a];
                pad[b] = after.pad[b];
            }
            break;
        }
        case SettingRow::RomFolder: {
            if (step != 0) return;
            const std::string folder = macui::chooseFolder(library_.folder());
            heldKeys_.clear();  // key-up events may have gone to the dialog
            if (folder.empty() || folder == library_.folder()) return;
            library_.setFolder(folder);
            settings_.romFolder = folder == defaultRomFolder() ? "" : folder;
            settings_.romFolderBookmark = settings_.romFolder.empty() ? "" : macui::makeFolderBookmark(folder);
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
        case SettingRow::Controls: return "Controls";
        case SettingRow::ConfirmButton: return "Menu confirm button";
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
        case SettingRow::Controls: return "Keyboard & controller \u203A";
        case SettingRow::ConfirmButton: return settings_.circleIsA ? "Circle \u25CB" : "Cross \u2715";
        case SettingRow::RomFolder: return shortPath(library_.folder());
        default: return "";
        }
    }

    // Rows that take left/right show ‹ › arrows around the value.
    static bool hasArrows(SettingRow row) {
        return row != SettingRow::RomFolder && row != SettingRow::Done && row != SettingRow::Controls;
    }

    // -----------------------------------------------------------------------
    // Controls screen (opened from Settings)
    // -----------------------------------------------------------------------
    //
    // One row per control with four cells: two keyboard keys and two
    // controller buttons. Pick a cell and press Return (or Cross), then the
    // key or button to use. Below them are options for the stick, triggers,
    // turbo and fast-forward, then "Reset to defaults" and "Done".

    int controlsRowCount() const { return kControlCount + kControlOptionCount + 1; }
    bool isBindingRow(int row) const { return row < kControlCount; }
    bool isOptionRow(int row) const { return row >= kControlCount && row < kControlCount + kControlOptionCount; }
    bool isButtonsRow(int row) const { return row == controlsRowCount() - 1; }
    bool captureIsKeyboard() const { return controlsCol_ < kBindingSlots; }

    void openControls() {
        screen_ = Screen::Controls;
        controlsRow_ = controlsCol_ = controlsScroll_ = 0;
        capturing_ = false;
    }

    void closeControls() {
        capturing_ = false;
        screen_ = Screen::Settings;
        settingsSelected_ = static_cast<int>(SettingRow::Controls);
    }

    void controlsAction(MenuAction action) {
        const int count = controlsRowCount();
        switch (action) {
        case MenuAction::Up: controlsRow_ = (controlsRow_ + count - 1) % count; break;
        case MenuAction::Down: controlsRow_ = (controlsRow_ + 1) % count; break;
        case MenuAction::Left:
        case MenuAction::Right: {
            const int step = action == MenuAction::Left ? -1 : 1;
            if (isOptionRow(controlsRow_)) changeControlOption(controlOption(controlsRow_), step);
            else controlsCol_ += step;
            break;
        }
        case MenuAction::Accept:
            if (isBindingRow(controlsRow_)) startCapture();
            else if (isOptionRow(controlsRow_)) changeControlOption(controlOption(controlsRow_), 0);
            else if (controlsCol_ == 0) {
                settings_.resetControls();
                fastForwardOn_ = false;
                saveSettings();
                showToast("Controls reset to defaults");
            } else {
                closeControls();
            }
            break;
        case MenuAction::Clear:
            if (isBindingRow(controlsRow_)) {
                int& slot = captureIsKeyboard() ? settings_.bindings.keys[controlsRow_][controlsCol_]
                                                : settings_.bindings.pad[controlsRow_][controlsCol_ - kBindingSlots];
                if (slot != kUnbound) {
                    slot = kUnbound;
                    saveSettings();
                }
            }
            break;
        case MenuAction::Back: case MenuAction::OpenSettings: closeControls(); break;
        default: break;
        }
        controlsCol_ = std::clamp(controlsCol_, 0, isButtonsRow(controlsRow_) ? 1 : 2 * kBindingSlots - 1);
    }

    static ControlOption controlOption(int row) { return static_cast<ControlOption>(row - kControlCount); }

    static const char* controlOptionLabel(ControlOption option) {
        switch (option) {
        case ControlOption::StickDpad: return "Left stick as D-pad";
        case ControlOption::StickDeadzone: return "Stick dead zone";
        case ControlOption::TriggerPoint: return "L2 / R2 press point";
        case ControlOption::TurboSpeed: return "Turbo speed";
        case ControlOption::FastForwardMode: return "Fast-forward button";
        case ControlOption::Count: break;
        }
        return "";
    }

    std::string controlOptionValue(ControlOption option) const {
        switch (option) {
        case ControlOption::StickDpad: return settings_.leftStickDpad ? "On" : "Off";
        case ControlOption::StickDeadzone: return std::to_string(settings_.stickDeadzone) + "%";
        case ControlOption::TriggerPoint: return std::to_string(settings_.triggerPoint) + "%";
        case ControlOption::TurboSpeed: {
            // A turbo press lasts turboFrames frames, then the release as long: 60 / (2 * frames) a second.
            const int f = settings_.turboFrames;
            return (f == 4 ? std::string("7.5") : std::to_string(30 / f)) + " presses/s";
        }
        case ControlOption::FastForwardMode: return settings_.fastForwardToggle ? "Press to toggle" : "Hold";
        case ControlOption::Count: break;
        }
        return "";
    }

    // `step` is -1 (left), +1 (right) or 0 (Return: next value, wrapping around).
    void changeControlOption(ControlOption option, int step) {
        auto percent = [step](int& value, int low, int high) {
            if (step == 0) value = value + 5 > high ? low : value + 5;
            else value = std::clamp(value + step * 5, low, high);
        };
        switch (option) {
        case ControlOption::StickDpad: settings_.leftStickDpad = !settings_.leftStickDpad; break;
        case ControlOption::StickDeadzone: percent(settings_.stickDeadzone, 10, 80); break;
        case ControlOption::TriggerPoint: percent(settings_.triggerPoint, 5, 95); break;
        case ControlOption::TurboSpeed: {
            // Right = faster = fewer frames per press. The list is slowest first.
            static constexpr int kSlowestFirst[] = {6, 4, 3, 2, 1};
            constexpr int n = static_cast<int>(std::size(kSlowestFirst));
            int i = 0;
            for (int k = 0; k < n; ++k)
                if (kSlowestFirst[k] == settings_.turboFrames) i = k;
            i = step == 0 ? (i + 1) % n : std::clamp(i + step, 0, n - 1);
            settings_.turboFrames = kSlowestFirst[i];
            break;
        }
        case ControlOption::FastForwardMode:
            settings_.fastForwardToggle = !settings_.fastForwardToggle;
            fastForwardOn_ = false;
            break;
        case ControlOption::Count: return;
        }
        saveSettings();
    }

    // ---- Picking a new key or button ----

    void startCapture() {
        capturing_ = true;
        captureUntil_ = SDL_GetTicks() + 8000;  // give up after 8 seconds
        captureTriggerDown_[0] = triggerDown(0);
        captureTriggerDown_[1] = triggerDown(1);
    }

    void stopCapture(const std::string& message) {
        capturing_ = false;
        if (!message.empty()) showToast(message);
    }

    // L2 and R2 are axes, so they are watched here rather than through button events.
    void updateCapture() {
        if (SDL_TICKS_PASSED(SDL_GetTicks(), captureUntil_)) return stopCapture("Nothing changed");
        if (captureIsKeyboard()) return;
        for (int side = 0; side < 2; ++side) {
            const bool down = triggerDown(side);
            if (down && !captureTriggerDown_[side]) return assignBinding(side == 0 ? kPadTriggerLeft : kPadTriggerRight);
            if (!down) captureTriggerDown_[side] = false;  // was held at the start: wait for a fresh press
        }
    }

    void captureKey(const SDL_KeyboardEvent& key) {
        const SDL_Keycode sym = key.keysym.sym;
        if (sym == SDLK_ESCAPE) return stopCapture("");  // Esc cancels, and always opens the pause menu in games
        if (key.keysym.mod & KMOD_GUI) return;           // leave Cmd shortcuts (like Cmd+Q) alone
        if (!captureIsKeyboard()) return;                // waiting for a controller button
        assignBinding(sym);
    }

    void capturePadButton(Uint8 button) {
        if (isReservedPadInput(button)) return stopCapture("");  // PS / mic cancel; they always open the menu
        if (captureIsKeyboard()) return;                         // waiting for a key
        assignBinding(button);
    }

    // Puts `input` in the selected cell. Each key or button does one thing,
    // so it's taken off any other control it was on.
    void assignBinding(int input) {
        const bool keyboard = captureIsKeyboard();
        auto& table = keyboard ? settings_.bindings.keys : settings_.bindings.pad;
        const int control = controlsRow_, slot = controlsCol_ % kBindingSlots;
        std::string movedFrom;
        for (int c = 0; c < kControlCount; ++c) {
            for (int s = 0; s < kBindingSlots; ++s) {
                if (table[c][s] != input || (c == control && s == slot)) continue;
                table[c][s] = kUnbound;
                if (c != control) movedFrom = controlName(static_cast<Control>(c));
            }
        }
        table[control][slot] = input;
        saveSettings();
        const std::string label = keyboard ? keyLabel(input) : padLabel(input);
        const std::string name = controlName(static_cast<Control>(control));
        stopCapture(movedFrom.empty() ? label + " \u2192 " + name
                                      : label + " \u2192 " + name + " (moved from " + movedFrom + ")");
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
        const macui::TextImage image = macui::renderText(text, static_cast<double>(size) * scale_, bold);
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
        auto width = [&](const std::string& s) {
            return macui::measureText(s, static_cast<double>(size) * scale_, bold) / scale_;
        };
        std::string result = text;
        if (width(result) > maxWidth) {
            std::string base = text;
            while (!base.empty()) {
                // Drop one whole UTF-8 character: its continuation bytes
                // (10xxxxxx) and then the byte that starts it.
                while (!base.empty()) {
                    const unsigned char byte = static_cast<unsigned char>(base.back());
                    base.pop_back();
                    if ((byte & 0xC0) != 0x80) break;
                }
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

    // Shown in the same corner while slow motion is on.
    void drawSlowMotionBadge() {
        const Rect box{14, 12, 52, 30};
        drawPixelBox(box, palette::kPanelSolid, withAlpha(palette::kGold, 0.6));
        drawTextIn(box, "\u00BD\u00D7", 14, palette::kGoldLight, true, Align::Center);
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

    // ---- Save States ----
    //
    // A list on the left (newest first, under a "Save a new state" row) and
    // the selected state's screenshot on the right, big enough to tell apart
    // two states taken in the same room.

    static constexpr float kStatesPitch = 58;

    struct StatesLayout {
        Rect panel, list, preview, mainButton, deleteButton;
        int visibleRows = 1;
    };

    StatesLayout statesLayout() const {
        const Rect win = windowRect();
        StatesLayout l;
        const float w = std::min(860.0f, win.w - 48), h = std::min(600.0f, win.h - 32);
        l.panel = {(win.w - w) / 2, (win.h - h) / 2, w, h};
        const float top = l.panel.y + 70, bottom = l.panel.y + h - 62;
        const float innerX = l.panel.x + 18, innerW = w - 36;
        const float listW = std::floor(innerW * 0.54f);
        l.list = {innerX, top, listW, bottom - top};
        l.preview = {innerX + listW + 14, top, innerW - listW - 14, bottom - top};
        l.visibleRows = std::max(1, static_cast<int>(l.list.h / kStatesPitch));
        const float buttonsW = std::min(244.0f, l.preview.w - 36);
        const float mainW = std::floor(buttonsW * 0.56f), y = l.preview.y + l.preview.h - 18 - 38;
        l.mainButton = {l.preview.x + l.preview.w - 18 - mainW, y, mainW, 38};
        l.deleteButton = {l.preview.x + l.preview.w - 18 - buttonsW, y, buttonsW - mainW - 10, 38};
        return l;
    }

    Rect statesRowRect(const StatesLayout& l, int row) const {
        return {l.list.x, l.list.y + (row - statesScroll_) * kStatesPitch, l.list.w - 10, kStatesPitch - 6};
    }

    // Keeps the selected row on screen.
    void scrollStatesToSelection(const StatesLayout& l) {
        if (statesSelected_ < statesScroll_) statesScroll_ = statesSelected_;
        if (statesSelected_ >= statesScroll_ + l.visibleRows) statesScroll_ = statesSelected_ - l.visibleRows + 1;
        statesScroll_ = std::clamp(statesScroll_, 0, std::max(0, statesRowCount() - l.visibleRows));
    }

    // The row under (x, y), or -1.
    int statesHitTest(float x, float y) const {
        const StatesLayout l = statesLayout();
        for (int row = statesScroll_; row < std::min(statesRowCount(), statesScroll_ + l.visibleRows); ++row)
            if (statesRowRect(l, row).contains(x, y)) return row;
        return -1;
    }

    // Short for the list; `full` for the preview, which has room to explain.
    static std::string stateKindLabel(StateKind kind, bool full = false) {
        if (kind == StateKind::Saved) return "Saved state";
        return full ? "Auto backup from before a load" : "Auto backup";
    }

    // "just now", "5 min ago", "3 h ago", "2 days ago".
    static std::string ageLabel(s64 createdMs) {
        const s64 seconds = std::max<s64>(0, (SaveStates::nowMs() - createdMs) / 1000);
        if (seconds < 60) return "just now";
        if (seconds < 3600) return std::to_string(seconds / 60) + " min ago";
        if (seconds < 86400) return std::to_string(seconds / 3600) + " h ago";
        const s64 days = seconds / 86400;
        return days == 1 ? "1 day ago" : std::to_string(days) + " days ago";
    }

    // How to quick save while playing, from the current key bindings.
    std::string quickSaveHint() const {
        const int key = settings_.bindings.keys[static_cast<int>(Control::QuickSave)][0];
        return "Quick save while playing: " + (key == kUnbound ? std::string() : keyLabel(key) + " or ") + "\u2318S";
    }

    // A screenshot in `r`, scaled from 240x160; a dim moon if there is none.
    void drawStateScreenshot(const Rect& r, const std::string& path, float moonUnit) {
        if (SDL_Texture* image = stateScreenshot(path)) {
            const SDL_Rect dst{static_cast<int>(std::lround(r.x * scale_)), static_cast<int>(std::lround(r.y * scale_)),
                               static_cast<int>(std::lround(r.w * scale_)), static_cast<int>(std::lround(r.h * scale_))};
            SDL_RenderCopy(renderer_, image, nullptr, &dst);
            return;
        }
        fill(r, {10, 10, 30, 230});
        const float mw = artSize(moonW_, moonUnit), mh = artSize(moonH_, moonUnit);
        drawMoon(r.x + (r.w - mw) / 2, r.y + (r.h - mh) / 2, moonUnit, 0.45);
    }

    void drawStates() {
        dimGame();
        const StatesLayout l = statesLayout();
        scrollStatesToSelection(l);
        drawPixelBox(l.panel, palette::kPanelSolid, palette::kBorder, 3);
        drawMenuTitle(l.panel, "Save States");

        constexpr float kThumbW = 60, kThumbH = 40;
        const int count = statesRowCount();
        for (int row = statesScroll_; row < std::min(count, statesScroll_ + l.visibleRows); ++row) {
            const Rect r = statesRowRect(l, row);
            const bool selected = row == statesSelected_;
            if (selected) drawSelection(r);
            else drawPixelBox(r, hovered(r) ? palette::kPanelHover : palette::kPanel);
            const Rect thumb{r.x + 22, r.y + (r.h - kThumbH) / 2, kThumbW, kThumbH};
            const float textX = thumb.x + kThumbW + 14, textW = r.x + r.w - 12 - textX;
            const Color detailColor = selected ? palette::kGoldLight : palette::kTextDim;
            const StateInfo* state = stateAt(row);
            if (!state) {
                // A gold "+" where the screenshot would be.
                drawPixelBox(thumb, withAlpha(palette::kGold, 0.16), withAlpha(palette::kGold, 0.6));
                drawTextIn(thumb, "+", 22, palette::kGoldLight, true, Align::Center);
                drawText(fitText("Save a new state", 15, true, textW), textX, r.y + 7, 15, palette::kText, true);
                drawText(fitText("Come back to this moment later", 12, false, textW), textX, r.y + 29, 12,
                         detailColor);
                continue;
            }
            drawStateScreenshot(thumb, state->path, 1);
            if (state->kind == StateKind::Backup)  // a thin border tells backups apart at a glance
                drawPixelBox({thumb.x - 2, thumb.y - 2, thumb.w + 4, thumb.h + 4}, {0, 0, 0, 0},
                             withAlpha(palette::kTextDim, 0.5), 1);
            drawText(fitText(stateTimeLabel(state->createdMs), 15, true, textW), textX, r.y + 7, 15, palette::kText,
                     true);
            drawText(fitText(stateKindLabel(state->kind) + "  \u00B7  " + ageLabel(state->createdMs), 12, false, textW),
                     textX, r.y + 29, 12, detailColor);
        }
        if (count == 1) {
            drawText(fitText("No save states yet.", 13, false, l.list.w - 20), l.list.x + l.list.w / 2,
                     l.list.y + kStatesPitch + 16, 13, palette::kTextDim, false, Align::Center);
        }
        if (count > l.visibleRows) {  // scrollbar
            const Rect track{l.list.x + l.list.w - 4, l.list.y, 4, l.visibleRows * kStatesPitch - 6};
            fill(track, {255, 255, 255, 20});
            const float thumbH = std::max(24.0f, track.h * l.visibleRows / count);
            const float thumbY = track.y + (track.h - thumbH) * statesScroll_ / std::max(1, count - l.visibleRows);
            fill({track.x, thumbY, track.w, thumbH}, withAlpha(palette::kGold, 0.75));
        }

        // Preview of the selected row: its screenshot, when it was made, and what can be done with it.
        const Rect& p = l.preview;
        drawPixelBox(p, palette::kPanel, palette::kBorder, 3);
        constexpr float kPad = 16, kBezel = 5, kInfoH = 124;
        const float maxW = p.w - 2 * (kPad + kBezel), maxH = p.h - 2 * (kPad + kBezel) - kInfoH;
        float w = maxW, h = w * kScreenHeight / kScreenWidth;
        if (h > maxH) { h = maxH; w = h * kScreenWidth / kScreenHeight; }
        const float whole = std::floor(w * scale_ / kScreenWidth);  // whole-number scaling keeps pixels sharp
        if (whole >= 1) { w = whole * kScreenWidth / scale_; h = whole * kScreenHeight / scale_; }
        const StateInfo* state = stateAt(statesSelected_);
        if (w > 0 && h > 0) {
            const Rect shot{p.x + (p.w - w) / 2, p.y + kPad + kBezel + (maxH - h) / 2, w, h};
            drawPixelBox({shot.x - kBezel, shot.y - kBezel, shot.w + 2 * kBezel, shot.h + 2 * kBezel}, {8, 8, 22},
                         {255, 255, 255, 30});
            drawStateScreenshot(shot, state ? state->path : "", 3);
        }
        const float textX = p.x + kPad, textW = p.w - 2 * kPad;
        const float infoY = l.mainButton.y - 18 - 52;
        if (state) {
            drawText(fitText(stateTimeLabel(state->createdMs), 17, true, textW), textX, infoY, 17, palette::kText, true);
            drawText(fitText(stateKindLabel(state->kind, true) + "  \u00B7  " + ageLabel(state->createdMs), 12, false,
                             textW),
                     textX, infoY + 28, 12, palette::kTextDim);
        } else {
            drawText(fitText("Right now", 17, true, textW), textX, infoY, 17, palette::kText, true);
            drawText(fitText(quickSaveHint(), 12, false, textW), textX, infoY + 28, 12, palette::kTextDim);
        }
        drawGoldButton(l.mainButton, state ? "\u25B6  Load" : "+  Save", 15, hovered(l.mainButton));
        if (state) {
            const bool armed = statesDeleteArmed_ == statesSelected_;
            if (armed) drawPixelBox(l.deleteButton, withAlpha(palette::kGold, 0.22), palette::kGold);
            else drawButton(l.deleteButton, "Delete");
            if (armed) drawTextIn(l.deleteButton, "Sure?", 14, palette::kGoldLight, true, Align::Center);
        }

        std::vector<Hint> hints;
        const char* accept = controller_ ? confirmGlyph() : "Return";
        hints.push_back({accept, state ? "Load" : "Save"});
        if (state) hints.push_back({controller_ ? "\u25B3" : "\u232B", "Delete"});
        hints.push_back({controller_ ? backGlyph() : "Esc", "Back"});
        drawHints(hints, l.panel.x + l.panel.w / 2, l.panel.y + l.panel.h - 30, l.panel.w - 24, Align::Center);
        drawToast();
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
            while (titleFitSize_ > 16 &&
                   macui::measureText(kWordmark, static_cast<double>(titleFitSize_) * scale_, true) / scale_ > maxWidth)
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
        drawText(std::string(kTagline) + "  |  Version " + LALUNE_VERSION, titleX + 1, 58, 10,
                 palette::kTextDim, true);
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

    // Space between settings rows: 42 points, less in short windows so every row fits.
    float settingsPitch() const {
        const float fit = (windowRect().h - 24 - 70 - 52) / static_cast<int>(SettingRow::Count);
        return std::clamp(std::floor(fit), 34.0f, 42.0f);
    }

    Rect settingsPanel() const {
        const Rect win = windowRect();
        const float w = std::min(580.0f, win.w - 48);
        const float h = 70 + static_cast<int>(SettingRow::Count) * settingsPitch() + 52;
        return {(win.w - w) / 2, (win.h - h) / 2, w, h};
    }

    Rect settingsRowRect(int i) const {
        const Rect panel = settingsPanel();
        const float pitch = settingsPitch();
        return {panel.x + 18, panel.y + 64 + i * pitch, panel.w - 36, pitch - 4};
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

    // ---- Controls ----

    static constexpr float kControlsPitch = 34;

    Rect controlsPanel() const {
        const Rect win = windowRect();
        const float w = std::min(780.0f, win.w - 48);
        const float content = 92 + controlsRowCount() * kControlsPitch + 62;
        const float h = std::min(content, win.h - 32);
        return {(win.w - w) / 2, (win.h - h) / 2, w, h};
    }

    float controlsListTop() const { return controlsPanel().y + 92; }

    int controlsVisibleRows() const {
        const Rect panel = controlsPanel();
        const float height = panel.y + panel.h - 62 - controlsListTop();
        return std::max(1, static_cast<int>(height / kControlsPitch));
    }

    // Keeps the selected row on screen.
    void scrollControlsToSelection() {
        const int visible = controlsVisibleRows();
        if (controlsRow_ < controlsScroll_) controlsScroll_ = controlsRow_;
        if (controlsRow_ >= controlsScroll_ + visible) controlsScroll_ = controlsRow_ - visible + 1;
        controlsScroll_ = std::clamp(controlsScroll_, 0, std::max(0, controlsRowCount() - visible));
    }

    Rect controlsRowRect(int row) const {
        const Rect panel = controlsPanel();
        const float y = controlsListTop() + (row - controlsScroll_) * kControlsPitch;
        return {panel.x + 18, y, panel.w - 36 - 10, kControlsPitch - 4};
    }

    // Everything right of the row's label: the four cells, or an option's value.
    static Rect controlsValueArea(const Rect& row) {
        const float labelW = std::max(132.0f, row.w * 0.27f);
        return {row.x + labelW, row.y, row.w - labelW - 4, row.h};
    }

    // Cells 0-1 are keyboard keys, 2-3 controller buttons (a wider gap between the groups).
    static Rect controlsCellRect(const Rect& row, int col) {
        const Rect area = controlsValueArea(row);
        constexpr float kGap = 6, kGroupGap = 16;
        const float w = (area.w - 2 * kGap - kGroupGap) / 4;
        const float x = area.x + col * (w + kGap) + (col >= 2 ? kGroupGap - kGap : 0);
        return {x, row.y + 2, w, row.h - 4};
    }

    Rect controlsButtonRect(int col) const {
        const Rect row = controlsRowRect(controlsRowCount() - 1);
        constexpr float kResetW = 190, kDoneW = 140, kGap = 14;
        const float x = row.x + (row.w - (kResetW + kGap + kDoneW)) / 2;
        return col == 0 ? Rect{x, row.y, kResetW, row.h} : Rect{x + kResetW + kGap, row.y, kDoneW, row.h};
    }

    // The row (and cell or button) under (x, y). Returns false if there's none.
    bool controlsHitTest(float x, float y, int& row, int& col) const {
        const int visible = controlsVisibleRows();
        for (int r = controlsScroll_; r < std::min(controlsRowCount(), controlsScroll_ + visible); ++r) {
            const Rect rect = controlsRowRect(r);
            if (!rect.contains(x, y)) continue;
            if (isButtonsRow(r)) {
                for (int c = 0; c < 2; ++c) {
                    if (controlsButtonRect(c).contains(x, y)) {
                        row = r;
                        col = c;
                        return true;
                    }
                }
                return false;
            }
            row = r;
            if (isBindingRow(r)) {
                for (int c = 0; c < 2 * kBindingSlots; ++c)
                    if (controlsCellRect(rect, c).contains(x, y)) col = c;
                col = std::clamp(col, 0, 2 * kBindingSlots - 1);
            }
            return true;
        }
        return false;
    }

    void drawControls() {
        const bool overGame = settingsReturn_ == Screen::Paused && gba_;
        if (overGame) dimGame();
        else drawScene();
        scrollControlsToSelection();
        const Rect panel = controlsPanel();
        drawPixelBox(panel, overGame ? palette::kPanelSolid : palette::kPanel, palette::kBorder, 3);
        drawMenuTitle(panel, "Controls");

        // Column headings over the cells.
        const Rect first = controlsRowRect(controlsScroll_);
        const Rect headerRow{first.x, panel.y + 64, first.w, 22};
        const Rect k0 = controlsCellRect(headerRow, 0), k1 = controlsCellRect(headerRow, 1);
        const Rect p0 = controlsCellRect(headerRow, 2), p1 = controlsCellRect(headerRow, 3);
        const char* keyboardTitle = "K\u2009E\u2009Y\u2009B\u2009O\u2009A\u2009R\u2009D";
        const char* controllerTitle = "C\u2009O\u2009N\u2009T\u2009R\u2009O\u2009L\u2009L\u2009E\u2009R";
        drawTextIn({k0.x, headerRow.y, k1.x + k1.w - k0.x, 22}, keyboardTitle, 10, palette::kTextDim, true,
                   Align::Center);
        drawTextIn({p0.x, headerRow.y, p1.x + p1.w - p0.x, 22}, controllerTitle, 10, palette::kTextDim, true,
                   Align::Center);

        const int visible = controlsVisibleRows();
        const double blink = 0.5 + 0.5 * std::sin(animTime() * 6);
        for (int row = controlsScroll_; row < std::min(controlsRowCount(), controlsScroll_ + visible); ++row) {
            const Rect r = controlsRowRect(row);
            const bool selectedRow = row == controlsRow_;
            if (isButtonsRow(row)) {
                for (int c = 0; c < 2; ++c) {
                    const Rect b = controlsButtonRect(c);
                    const char* label = c == 0 ? "Reset to defaults" : "Done";
                    if (selectedRow && controlsCol_ == c) drawGoldButton(b, label, 14, false);
                    else drawButton(b, label);
                }
                continue;
            }
            if (isOptionRow(row)) {
                if (row == kControlCount) fill({r.x, r.y - 3, r.w, 1}, palette::kBorder);  // divider above the options
                if (selectedRow) drawSelection(r);
                const ControlOption option = controlOption(row);
                drawTextIn(r, controlOptionLabel(option), 14, selectedRow ? palette::kText : palette::kTextDim,
                           selectedRow, Align::Left, 24);
                const Rect value = controlsValueArea(r);
                drawTextIn(value, controlOptionValue(option), 14, selectedRow ? palette::kGoldLight : palette::kText,
                           true, Align::Center);
                const Color arrow = selectedRow ? palette::kGold : withAlpha(palette::kTextDim, 0.7);
                drawTextIn({value.x, r.y, 32, r.h}, "\u2039", 20, arrow, true, Align::Center);
                drawTextIn({value.x + value.w - 32, r.y, 32, r.h}, "\u203A", 20, arrow, true, Align::Center);
                continue;
            }

            // A control and its four cells.
            if (selectedRow) {
                drawPixelBox(r, palette::kPanelHover);
                drawCursor(r.x + 8, r.y + r.h / 2);
            }
            drawTextIn(r, controlName(static_cast<Control>(row)), 14, selectedRow ? palette::kText : palette::kTextDim,
                       selectedRow, Align::Left, 24);
            for (int col = 0; col < 2 * kBindingSlots; ++col) {
                const Rect cell = controlsCellRect(r, col);
                const bool selected = selectedRow && col == controlsCol_;
                const int input = col < kBindingSlots ? settings_.bindings.keys[row][col]
                                                      : settings_.bindings.pad[row][col - kBindingSlots];
                if (selected && capturing_) {
                    drawPixelBox(cell, withAlpha(palette::kGold, 0.18 + 0.2 * blink), palette::kGold);
                    std::string prompt = col < kBindingSlots ? "Press a key\u2026" : "Press a button\u2026";
                    if (textWidth(prompt, 12, true) > cell.w - 8) prompt = "Press\u2026";  // narrow window
                    drawTextIn(cell, prompt, 12, palette::kGoldLight, true, Align::Center);
                    continue;
                }
                if (selected) drawPixelBox(cell, palette::kPanelHover, palette::kGold);
                else drawPixelBox(cell, {255, 255, 255, 14}, {255, 255, 255, 30});
                const std::string label = col < kBindingSlots ? keyLabel(input) : padLabel(input);
                if (label.empty()) {
                    drawTextIn(cell, "\u2014", 13, withAlpha(palette::kTextDim, 0.5), false, Align::Center);
                } else {
                    drawTextIn(cell, fitText(label, 13, true, cell.w - 10), 13,
                               selected ? palette::kGoldLight : palette::kText, true, Align::Center);
                }
            }
        }

        if (controlsRowCount() > visible) {  // scrollbar
            const Rect track{panel.x + panel.w - 18 - 4, controlsListTop(), 4, visible * kControlsPitch - 4};
            fill(track, {255, 255, 255, 20});
            const float thumbH = std::max(24.0f, track.h * visible / controlsRowCount());
            const float thumbY =
                track.y + (track.h - thumbH) * controlsScroll_ / std::max(1, controlsRowCount() - visible);
            fill({track.x, thumbY, track.w, thumbH}, withAlpha(palette::kGold, 0.75));
        }

        std::vector<Hint> hints;
        if (capturing_) {
            if (!captureIsKeyboard() && controller_) hints.push_back({"PS", "Cancel"});
            hints.push_back({"Esc", "Cancel"});
        } else if (isBindingRow(controlsRow_)) {
            hints = controller_ ? std::vector<Hint>{{confirmGlyph(), "Change"}, {"\u25B3", "Clear"},
                                                    {backGlyph(), "Back"}}
                                : std::vector<Hint>{{"Return", "Change"}, {"\u232B", "Clear"}, {"Esc", "Back"}};
        } else if (isOptionRow(controlsRow_)) {
            hints = {{"\u2190 \u2192", "Change"}, {controller_ ? backGlyph() : "Esc", "Back"}};
        } else {
            hints = {{controller_ ? confirmGlyph() : "Return", "Select"}, {controller_ ? backGlyph() : "Esc", "Back"}};
        }
        drawHints(hints, panel.x + panel.w / 2, panel.y + panel.h - 30, panel.w - 24, Align::Center);
        drawToast();
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
            screen_ = Screen::Controls;
            controlsRow_ = static_cast<int>(Control::R);
            controlsCol_ = 3;
            controlsScroll_ = 0;
            shot("controls", [&] { drawControls(); });
            capturing_ = true;
            shot("controls-capture", [&] { drawControls(); });
            capturing_ = false;
            controlsRow_ = kControlCount + static_cast<int>(ControlOption::TurboSpeed);
            shot("controls-options", [&] { drawControls(); });
            if (gba_) {
                screen_ = Screen::Paused;
                pauseSelected_ = 0;
                shot("pause", [&] { drawPauseMenu(); });
                screen_ = Screen::Settings;
                settingsReturn_ = Screen::Paused;
                shot("settings-over-game", [&] { drawSettings(); });
                // Save States, with a few states kept in the dump folder (not the real one).
                screen_ = Screen::States;
                states_.open((fs::path(dir) / "states").string(), gba_->romHash());
                std::string error;
                if (states_.list().size() < 3) {
                    for (int i = 0; i < 3; ++i) {
                        for (int f = 0; f < 60; ++f) gba_->runFrame();
                        states_.save(*gba_, i == 2 ? StateKind::Backup : StateKind::Saved, error);
                    }
                }
                gba_->apu.samples.clear();
                clearStateScreenshots();
                statesScroll_ = 0;
                statesDeleteArmed_ = -1;
                statesSelected_ = 0;
                shot("states-new", [&] { drawStates(); });
                statesSelected_ = std::min(2, statesRowCount() - 1);
                shot("states", [&] { drawStates(); });
                statesDeleteArmed_ = statesSelected_;
                shot("states-delete", [&] { drawStates(); });
                statesDeleteArmed_ = -1;
                screen_ = Screen::Paused;
                pauseSelected_ = static_cast<int>(PauseItem::SaveStates);
                shot("pause-states-item", [&] { drawPauseMenu(); });
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
    std::string romPath, dumpPath, savePath, uiDumpDir, loadStatePath, saveStatePath;
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
        else if (arg == "--load-state" && i + 1 < argc) loadStatePath = argv[++i];
        else if (arg == "--save-state" && i + 1 < argc) saveStatePath = argv[++i];
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
            if (!loadStatePath.empty()) {
                std::string error;
                const auto file = SaveStates::readFile(loadStatePath, *gba, error);
                if (!file || !SaveStates::apply(*gba, *file)) {
                    std::fprintf(stderr, "Could not load state '%s': %s\n", loadStatePath.c_str(),
                                 error.empty() ? "it doesn't fit this program" : error.c_str());
                    result = 1;
                } else {
                    std::printf("State loaded from %s\n", loadStatePath.c_str());
                }
            }
            if (result == 0) {
                result = runHeadless(*gba, headlessFrames, dumpPath, presses, dumpEvery,
                                     std::clamp(dumpFilter, -1, kVideoFilterCount - 1));
            }
            if (result == 0 && !saveStatePath.empty()) {
                std::string error;
                if (SaveStates::writeFile(saveStatePath, *gba, StateKind::Saved, SaveStates::nowMs(), error)) {
                    std::printf("State written to %s\n", saveStatePath.c_str());
                } else {
                    std::fprintf(stderr, "Could not write state '%s': %s\n", saveStatePath.c_str(), error.c_str());
                    result = 1;
                }
            }
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
