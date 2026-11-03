// library.hpp - The game library: a folder of .gba files the launcher lists.
//
// ROMs added through the app are copied into the library folder, so the
// originals can be moved or deleted. Each game can also have a thumbnail (a
// screenshot taken when you last stopped playing it).
#pragma once

#include <optional>
#include <string>
#include <vector>

struct RomEntry {
    std::string path;   // full path to the .gba file
    std::string name;   // file name without extension, shown as the game's name
    std::string title;  // 12-character title from the ROM header
    std::string code;   // 4-character game code from the header, e.g. "AGBE"
};

// Reads the header of a ROM file. Returns nothing if it isn't a plausible GBA ROM.
std::optional<RomEntry> readRomInfo(const std::string& path);

class Library {
public:
    Library(std::string folder, std::string thumbnailFolder);

    void refresh();
    // Points the library at a different folder (created if needed) and rescans it.
    void setFolder(std::string folder);
    const std::vector<RomEntry>& games() const { return games_; }
    const std::string& folder() const { return folder_; }
    int indexOf(const std::string& path) const;

    // Copies a ROM into the library. Returns its new path, or nothing and
    // sets `error` if it couldn't be added.
    std::optional<std::string> add(const std::string& sourcePath, std::string& error);

    std::string thumbnailPath(const std::string& romPath) const;

private:
    std::string folder_, thumbnailFolder_;
    std::vector<RomEntry> games_;
};
