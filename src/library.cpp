#include "library.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kHeaderSize = 0xC0;
constexpr std::uintmax_t kMaxRomSize = 32 * 1024 * 1024;

bool hasGbaExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".gba";
}

// Header text is fixed-width ASCII padded with NULs or spaces.
std::string headerText(const char* data, std::size_t length) {
    std::string text;
    for (std::size_t i = 0; i < length && data[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        text += std::isprint(c) ? static_cast<char>(c) : ' ';
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

std::optional<RomEntry> readRomInfo(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size < kHeaderSize || size > kMaxRomSize) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    char header[kHeaderSize];
    if (!file.read(header, sizeof header)) return std::nullopt;
    RomEntry entry;
    entry.path = path;
    entry.name = fs::path(path).stem().string();
    entry.title = headerText(header + 0xA0, 12);
    entry.code = headerText(header + 0xAC, 4);
    return entry;
}

Library::Library(std::string folder, std::string thumbnailFolder)
    : folder_(std::move(folder)), thumbnailFolder_(std::move(thumbnailFolder)) {
    std::error_code ec;
    fs::create_directories(folder_, ec);
    fs::create_directories(thumbnailFolder_, ec);
}

void Library::setFolder(std::string folder) {
    folder_ = std::move(folder);
    std::error_code ec;
    fs::create_directories(folder_, ec);
    refresh();
}

void Library::refresh() {
    games_.clear();
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(folder_, ec)) {
        if (!item.is_regular_file(ec) || !hasGbaExtension(item.path())) continue;
        if (auto entry = readRomInfo(item.path().string())) games_.push_back(std::move(*entry));
    }
    std::sort(games_.begin(), games_.end(),
              [](const RomEntry& a, const RomEntry& b) { return lowercase(a.name) < lowercase(b.name); });
}

int Library::indexOf(const std::string& path) const {
    for (std::size_t i = 0; i < games_.size(); ++i)
        if (games_[i].path == path) return static_cast<int>(i);
    return -1;
}

std::optional<std::string> Library::add(const std::string& sourcePath, std::string& error) {
    const fs::path source(sourcePath);
    if (!hasGbaExtension(source)) {
        error = source.filename().string() + " is not a .gba file.";
        return std::nullopt;
    }
    if (!readRomInfo(sourcePath)) {
        error = source.filename().string() + " doesn't look like a GBA ROM.";
        return std::nullopt;
    }

    std::error_code ec;
    fs::path dest = fs::path(folder_) / source.filename();
    for (int n = 2; fs::exists(dest, ec); ++n) {
        // Already in the library (same file, or an identical-size copy): reuse it.
        if (fs::equivalent(source, dest, ec) || fs::file_size(source, ec) == fs::file_size(dest, ec)) {
            return dest.string();
        }
        dest = fs::path(folder_) / (source.stem().string() + " (" + std::to_string(n) + ").gba");
    }
    if (!fs::copy_file(source, dest, ec)) {
        error = "Couldn't copy " + source.filename().string() + ": " + ec.message();
        return std::nullopt;
    }
    return dest.string();
}

std::string Library::thumbnailPath(const std::string& romPath) const {
    return (fs::path(thumbnailFolder_) / (fs::path(romPath).stem().string() + ".bmp")).string();
}
