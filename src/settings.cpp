#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "video_filters.hpp"

namespace {

bool parseInt(const std::string& text, int& out) {
    try {
        std::size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used != text.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

// "120,-1" -> {120, -1}. Missing or bad entries become kUnbound.
Bindings::Slots parseSlots(const std::string& text) {
    Bindings::Slots slots;
    slots.fill(kUnbound);
    std::stringstream list(text);
    std::string item;
    for (int i = 0; i < kBindingSlots && std::getline(list, item, ','); ++i) {
        int value = kUnbound;
        if (parseInt(item, value)) slots[i] = value;
    }
    return slots;
}

std::string formatSlots(const Bindings::Slots& slots) {
    std::string text;
    for (int i = 0; i < kBindingSlots; ++i) text += (i ? "," : "") + std::to_string(slots[i]);
    return text;
}

}  // namespace

void Settings::resetControls() {
    const Settings defaults;
    bindings = Bindings::defaults(circleIsA);
    leftStickDpad = defaults.leftStickDpad;
    stickDeadzone = defaults.stickDeadzone;
    triggerPoint = defaults.triggerPoint;
    turboFrames = defaults.turboFrames;
    fastForwardToggle = defaults.fastForwardToggle;
}

Settings Settings::load(const std::string& path) {
    Settings s;
    std::vector<std::pair<std::string, std::string>> lines;
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) lines.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        }
    }
    for (const auto& [key, value] : lines) {
        int number = 0;
        if (key == "volume" && parseInt(value, number)) s.volume = std::clamp(number, 0, 100);
        else if (key == "fast_forward_speed" && parseInt(value, number)) {
            if (std::find(std::begin(kFastForwardSpeeds), std::end(kFastForwardSpeeds), number) !=
                std::end(kFastForwardSpeeds))
                s.fastForwardSpeed = number;
        } else if (key == "pixel_perfect") s.pixelPerfect = value != "0";
        else if (key == "video_filter" && parseInt(value, number) && number >= 0 && number < kVideoFilterCount)
            s.videoFilter = number;
        else if (key == "fullscreen") s.fullscreen = value == "1";
        else if (key == "circle_is_a") s.circleIsA = value == "1";
        else if (key == "rom_folder") s.romFolder = value;
        else if (key == "rom_folder_bookmark") s.romFolderBookmark = value;
        else if (key == "left_stick_dpad") s.leftStickDpad = value != "0";
        else if (key == "stick_deadzone" && parseInt(value, number)) s.stickDeadzone = std::clamp(number, 10, 80);
        else if (key == "trigger_point" && parseInt(value, number)) s.triggerPoint = std::clamp(number, 5, 95);
        else if (key == "turbo_frames" && parseInt(value, number)) {
            if (std::find(std::begin(kTurboFrames), std::end(kTurboFrames), number) != std::end(kTurboFrames))
                s.turboFrames = number;
        } else if (key == "fast_forward_toggle") s.fastForwardToggle = value == "1";
    }

    // Controls. Before they could be changed, "circle_is_a" also swapped A and
    // B on the controller, so start from the layout that matches it.
    s.bindings = Bindings::defaults(s.circleIsA);
    for (const auto& [key, value] : lines) {
        for (int c = 0; c < kControlCount; ++c) {
            const std::string id = controlId(static_cast<Control>(c));
            if (key == "key_" + id) s.bindings.keys[c] = parseSlots(value);
            else if (key == "pad_" + id) s.bindings.pad[c] = parseSlots(value);
        }
    }
    return s;
}

bool Settings::save(const std::string& path) const {
    // Written to a temporary file first and then swapped in, so a crash or a
    // full disk can't leave a half-written settings.ini behind.
    const std::string temp = path + ".tmp";
    std::ofstream file(temp, std::ios::trunc);
    if (!file) return false;
    file << "volume=" << volume << '\n'
         << "fast_forward_speed=" << fastForwardSpeed << '\n'
         << "pixel_perfect=" << (pixelPerfect ? 1 : 0) << '\n'
         << "video_filter=" << videoFilter << '\n'
         << "fullscreen=" << (fullscreen ? 1 : 0) << '\n'
         << "circle_is_a=" << (circleIsA ? 1 : 0) << '\n'
         << "rom_folder=" << romFolder << '\n'
         << "rom_folder_bookmark=" << romFolderBookmark << '\n'
         << "left_stick_dpad=" << (leftStickDpad ? 1 : 0) << '\n'
         << "stick_deadzone=" << stickDeadzone << '\n'
         << "trigger_point=" << triggerPoint << '\n'
         << "turbo_frames=" << turboFrames << '\n'
         << "fast_forward_toggle=" << (fastForwardToggle ? 1 : 0) << '\n';
    // Keys are SDL key codes and controller inputs are SDL button numbers
    // (100 = L2, 101 = R2); -1 means nothing is assigned.
    for (int c = 0; c < kControlCount; ++c) {
        const std::string id = controlId(static_cast<Control>(c));
        file << "key_" << id << '=' << formatSlots(bindings.keys[c]) << '\n'
             << "pad_" << id << '=' << formatSlots(bindings.pad[c]) << '\n';
    }
    file.close();
    if (!file || std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}
