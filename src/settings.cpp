#include "settings.hpp"

#include <algorithm>
#include <fstream>

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

}  // namespace

Settings Settings::load(const std::string& path) {
    Settings s;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
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
    }
    return s;
}

bool Settings::save(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "volume=" << volume << '\n'
         << "fast_forward_speed=" << fastForwardSpeed << '\n'
         << "pixel_perfect=" << (pixelPerfect ? 1 : 0) << '\n'
         << "video_filter=" << videoFilter << '\n'
         << "fullscreen=" << (fullscreen ? 1 : 0) << '\n'
         << "circle_is_a=" << (circleIsA ? 1 : 0) << '\n'
         << "rom_folder=" << romFolder << '\n';
    return static_cast<bool>(file);
}
