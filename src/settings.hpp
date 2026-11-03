// settings.hpp - User preferences, saved as a small text file between runs.
//
// The file is plain "key=value" lines so it is easy to read and edit by hand.
// Unknown keys are ignored and missing ones keep their defaults, so old
// settings files keep working when new options are added.
#pragma once

#include <string>

struct Settings {
    int volume = 80;             // 0-100 percent, 0 = muted
    int fastForwardSpeed = 4;    // frames emulated per displayed frame while fast-forwarding
    bool pixelPerfect = true;    // true: whole-number scaling (sharp), false: fill the window
    int videoFilter = 1;         // a VideoFilter value (see video_filters.hpp); 1 = Smooth HD
    bool fullscreen = false;     // start in (and stay in) full screen
    bool circleIsA = false;      // false: Cross = A (PlayStation style), true: Circle = A (Nintendo position)
    std::string romFolder;       // where the library keeps games; empty = default location

    static constexpr int kFastForwardSpeeds[] = {2, 3, 4, 6, 8};

    // Reads settings from `path`. Missing file or bad values fall back to defaults.
    static Settings load(const std::string& path);
    bool save(const std::string& path) const;
};
