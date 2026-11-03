// settings.hpp - User preferences, saved as a small text file between runs.
//
// The file is plain "key=value" lines so it is easy to read and edit by hand.
// Unknown keys are ignored and missing ones keep their defaults, so old
// settings files keep working when new options are added.
#pragma once

#include <string>

#include "controls.hpp"

struct Settings {
    int volume = 80;             // 0-100 percent, 0 = muted
    int fastForwardSpeed = 4;    // frames emulated per displayed frame while fast-forwarding
    bool pixelPerfect = true;    // true: whole-number scaling (sharp), false: fill the window
    int videoFilter = 1;         // a VideoFilter value (see video_filters.hpp); 1 = Smooth HD
    bool fullscreen = false;     // start in (and stay in) full screen
    bool circleIsA = false;      // menus: false = Cross confirms, true = Circle confirms (Nintendo position)
    std::string romFolder;       // where the library keeps games; empty = default location
    std::string romFolderBookmark;  // macOS permission to open romFolder from the sandbox (see mac_ui.hpp)

    // Controls (see the Controls screen).
    Bindings bindings = Bindings::defaults(false);
    bool leftStickDpad = true;    // the left stick also works as the D-pad
    int stickDeadzone = 35;       // percent of the stick's travel before it presses a direction
    int triggerPoint = 25;        // percent of L2/R2 travel where they count as pressed
    int turboFrames = 3;          // turbo buttons press for this many frames, then release for as many
    bool fastForwardToggle = false;  // false: hold to fast-forward, true: press once to start, again to stop

    static constexpr int kFastForwardSpeeds[] = {2, 3, 4, 6, 8};
    static constexpr int kTurboFrames[] = {1, 2, 3, 4, 6};  // 30, 15, 10, 7.5 and 5 presses a second

    // Puts the controls back to their defaults (keeps the menu confirm button).
    void resetControls();

    // Reads settings from `path`. Missing file or bad values fall back to defaults.
    static Settings load(const std::string& path);
    bool save(const std::string& path) const;
};
