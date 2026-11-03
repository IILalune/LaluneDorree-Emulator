// mac_ui.hpp - Small bits of native macOS UI that SDL doesn't provide.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace macui {

// White text with an alpha channel, as 0xAARRGGBB pixels (SDL_PIXELFORMAT_ARGB8888).
struct TextImage {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};

// Renders one line of text with the rounded system font. `pixelSize` is in real
// screen pixels, so pass a bigger value on Retina displays.
TextImage renderText(const std::string& utf8, double pixelSize, bool bold);

// Width in pixels that renderText would produce, without drawing anything.
double measureText(const std::string& utf8, double pixelSize, bool bold);

// Shows the standard "Open" dialog for .gba files. Returns the chosen paths
// (empty if cancelled).
std::vector<std::string> chooseRomFiles();

// Shows an "Open" dialog for picking a folder, starting in `current`.
// Returns the chosen path, or an empty string if cancelled.
std::string chooseFolder(const std::string& current);

// Opens a folder in Finder.
void openInFinder(const std::string& folder);

}  // namespace macui
