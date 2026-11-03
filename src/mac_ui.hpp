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

// The downloadable app runs in the App Sandbox, which only lets it open a folder
// outside its own while you've allowed it (by picking it in an Open dialog).
// A bookmark keeps that permission between launches.
// Makes a bookmark (as text, for settings.ini) for a folder you just picked.
// Returns "" on failure.
std::string makeFolderBookmark(const std::string& folder);
// Regains access to a bookmarked folder and returns its current path ("" if
// the folder is gone or the bookmark is invalid). `bookmark` is updated if
// macOS asks for a fresher one.
std::string openFolderBookmark(std::string& bookmark);

// Stops macOS from taking controller buttons for its own shortcuts while the
// app is in front (the PS button would open the Games overlay). Covers
// controllers that connect later too. Safe to call more than once.
void keepControllerButtonsFromSystem();

}  // namespace macui
