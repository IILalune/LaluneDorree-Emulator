// controls.hpp - Which keys and controller buttons press which GBA button.
//
// Every control (a GBA button, or an extra like fast-forward or turbo) can
// have two keyboard keys and two controller buttons. They are changed on the
// Controls screen and saved in settings.ini.
#pragma once

#include <array>
#include <string>

// In the order the Controls screen lists them.
enum class Control { Up, Down, Left, Right, A, B, L, R, Start, Select, TurboA, TurboB, FastForward, SlowMotion,
                     QuickSave, QuickLoad, Count };
constexpr int kControlCount = static_cast<int>(Control::Count);
constexpr int kBindingSlots = 2;  // two keys and two controller buttons per control
constexpr int kUnbound = -1;

// Controller inputs are SDL_GameControllerButton numbers, plus these two for
// the analogue L2 and R2 triggers (SDL reports those as axes, not buttons).
constexpr int kPadTriggerLeft = 100;
constexpr int kPadTriggerRight = 101;

struct Bindings {
    using Slots = std::array<int, kBindingSlots>;
    std::array<Slots, kControlCount> keys{};  // SDL_Keycode values, or kUnbound
    std::array<Slots, kControlCount> pad{};   // controller inputs (see above), or kUnbound

    // The standard layout. `circleIsA` puts A on Circle and B on Cross and
    // Square, where A and B sit on a Nintendo pad.
    static Bindings defaults(bool circleIsA);
};

const char* controlName(Control control);  // shown on the Controls screen, e.g. "Fast-forward"
const char* controlId(Control control);    // used in settings.ini, e.g. "fast_forward"
std::string keyLabel(int keycode);         // e.g. "Right Shift", "\u2190"
std::string padLabel(int input);           // e.g. "Cross \u2715", "L2"

// The PS and mic buttons always open the pause menu, so they can't be assigned.
bool isReservedPadInput(int input);
