#include "controls.hpp"

#include <SDL.h>

Bindings Bindings::defaults(bool circleIsA) {
    Bindings b;
    for (auto& slots : b.keys) slots.fill(kUnbound);
    for (auto& slots : b.pad) slots.fill(kUnbound);
    auto set = [&](Control c, int key1, int key2, int pad1, int pad2) {
        const int i = static_cast<int>(c);
        b.keys[i] = {key1, key2};
        b.pad[i] = {pad1, pad2};
    };
    // SDL names face buttons by position (Xbox style): A = bottom (Cross),
    // B = right (Circle), X = left (Square), Y = top (Triangle). Cross is A
    // because it's the main button on PlayStation; Square is B because it
    // sits left of Cross, like B sits left of A on a GBA.
    const int a = circleIsA ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
    const int b2 = circleIsA ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
    set(Control::Up, SDLK_UP, kUnbound, SDL_CONTROLLER_BUTTON_DPAD_UP, kUnbound);
    set(Control::Down, SDLK_DOWN, kUnbound, SDL_CONTROLLER_BUTTON_DPAD_DOWN, kUnbound);
    set(Control::Left, SDLK_LEFT, kUnbound, SDL_CONTROLLER_BUTTON_DPAD_LEFT, kUnbound);
    set(Control::Right, SDLK_RIGHT, kUnbound, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kUnbound);
    set(Control::A, SDLK_x, kUnbound, a, kUnbound);
    set(Control::B, SDLK_z, kUnbound, SDL_CONTROLLER_BUTTON_X, b2);
    set(Control::L, SDLK_a, kUnbound, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kPadTriggerLeft);
    set(Control::R, SDLK_s, kUnbound, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kPadTriggerRight);
    set(Control::Start, SDLK_RETURN, kUnbound, SDL_CONTROLLER_BUTTON_START, kUnbound);
    set(Control::Select, SDLK_BACKSPACE, SDLK_RSHIFT, SDL_CONTROLLER_BUTTON_BACK, kUnbound);
    // Turbo buttons start unassigned: pick your own on the Controls screen.
    set(Control::TurboA, kUnbound, kUnbound, kUnbound, kUnbound);
    set(Control::TurboB, kUnbound, kUnbound, kUnbound, kUnbound);
    set(Control::FastForward, SDLK_TAB, kUnbound, SDL_CONTROLLER_BUTTON_TOUCHPAD, kUnbound);
    set(Control::SlowMotion, SDLK_BACKQUOTE, kUnbound, SDL_CONTROLLER_BUTTON_RIGHTSTICK, kUnbound);
    return b;
}

const char* controlName(Control control) {
    switch (control) {
    case Control::Up: return "Up";
    case Control::Down: return "Down";
    case Control::Left: return "Left";
    case Control::Right: return "Right";
    case Control::A: return "A";
    case Control::B: return "B";
    case Control::L: return "L";
    case Control::R: return "R";
    case Control::Start: return "Start";
    case Control::Select: return "Select";
    case Control::TurboA: return "Turbo A";
    case Control::TurboB: return "Turbo B";
    case Control::FastForward: return "Fast-forward";
    case Control::SlowMotion: return "Slow motion";
    case Control::Count: break;
    }
    return "";
}

const char* controlId(Control control) {
    switch (control) {
    case Control::Up: return "up";
    case Control::Down: return "down";
    case Control::Left: return "left";
    case Control::Right: return "right";
    case Control::A: return "a";
    case Control::B: return "b";
    case Control::L: return "l";
    case Control::R: return "r";
    case Control::Start: return "start";
    case Control::Select: return "select";
    case Control::TurboA: return "turbo_a";
    case Control::TurboB: return "turbo_b";
    case Control::FastForward: return "fast_forward";
    case Control::SlowMotion: return "slow_motion";
    case Control::Count: break;
    }
    return "";
}

std::string keyLabel(int keycode) {
    switch (keycode) {
    case kUnbound: return "";
    case SDLK_UP: return "\u2191";
    case SDLK_DOWN: return "\u2193";
    case SDLK_LEFT: return "\u2190";
    case SDLK_RIGHT: return "\u2192";
    case SDLK_LALT: return "Left \u2325";
    case SDLK_RALT: return "Right \u2325";
    case SDLK_LCTRL: return "Left Ctrl";
    case SDLK_RCTRL: return "Right Ctrl";
    case SDLK_BACKQUOTE: return "` (backtick)";
    default: break;
    }
    const char* name = SDL_GetKeyName(static_cast<SDL_Keycode>(keycode));
    return name && *name ? std::string(name) : "Key " + std::to_string(keycode);
}

std::string padLabel(int input) {
    switch (input) {
    case kUnbound: return "";
    case SDL_CONTROLLER_BUTTON_A: return "Cross \u2715";
    case SDL_CONTROLLER_BUTTON_B: return "Circle \u25CB";
    case SDL_CONTROLLER_BUTTON_X: return "Square \u25A1";
    case SDL_CONTROLLER_BUTTON_Y: return "Triangle \u25B3";
    case SDL_CONTROLLER_BUTTON_BACK: return "Create";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "PS";
    case SDL_CONTROLLER_BUTTON_START: return "Options";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L1";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R1";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad \u2191";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad \u2193";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad \u2190";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad \u2192";
    case SDL_CONTROLLER_BUTTON_MISC1: return "Mic";
    case SDL_CONTROLLER_BUTTON_PADDLE1: return "Paddle 1";
    case SDL_CONTROLLER_BUTTON_PADDLE2: return "Paddle 2";
    case SDL_CONTROLLER_BUTTON_PADDLE3: return "Paddle 3";
    case SDL_CONTROLLER_BUTTON_PADDLE4: return "Paddle 4";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad";
    case kPadTriggerLeft: return "L2";
    case kPadTriggerRight: return "R2";
    default: return "Button " + std::to_string(input);
    }
}

bool isReservedPadInput(int input) {
    return input == SDL_CONTROLLER_BUTTON_GUIDE || input == SDL_CONTROLLER_BUTTON_MISC1;
}
