#include "save_states.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "gba.hpp"
#include "state.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::array<char, 8> kMagic = {'L', 'L', 'D', 'S', 'T', 'A', 'T', 'E'};
constexpr u32 kFileVersion = 1;  // layout of the header and screenshot
constexpr std::size_t kHeaderSize = 56;
constexpr std::size_t kScreenshotPixels = kScreenWidth * kScreenHeight;
constexpr std::size_t kScreenshotBytes = kScreenshotPixels * sizeof(u32);
// A console state is about 420 KiB; anything far bigger isn't one of ours.
constexpr u64 kMaxPayload = 4 * 1024 * 1024;
constexpr const char* kExtension = ".state";

struct Header {
    std::array<char, 8> magic = kMagic;
    u32 fileVersion = kFileVersion;
    u32 stateVersion = GBA::kStateVersion;
    u32 kind = 0;
    u32 reserved = 0;
    u64 romHash = 0;
    s64 createdMs = 0;
    u64 payloadSize = 0;
    u64 contentHash = 0;  // of the screenshot and payload
};

void serializeHeader(StateStream& s, Header& h) {
    s.value(h.magic);
    s.value(h.fileVersion);
    s.value(h.stateVersion);
    s.value(h.kind);
    s.value(h.reserved);
    s.value(h.romHash);
    s.value(h.createdMs);
    s.value(h.payloadSize);
    s.value(h.contentHash);
}

// Reads and checks just the header. Returns false if this isn't a state file.
bool readHeader(std::ifstream& file, Header& header) {
    std::array<u8, kHeaderSize> bytes{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
    StateStream in(bytes.data(), bytes.size());
    serializeHeader(in, header);
    return in.ok() && in.atEnd() && header.magic == kMagic && header.fileVersion == kFileVersion &&
           (header.kind == static_cast<u32>(StateKind::Saved) || header.kind == static_cast<u32>(StateKind::Backup));
}

}  // namespace

s64 SaveStates::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void SaveStates::open(std::string folder, u64 romHash) {
    folder_ = std::move(folder);
    romHash_ = romHash;
    refresh();
}

void SaveStates::refresh() {
    states_.clear();
    if (folder_.empty()) return;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(folder_, ec)) {
        if (!item.is_regular_file(ec) || item.path().extension() != kExtension) continue;
        std::ifstream file(item.path(), std::ios::binary);
        Header header;
        if (!readHeader(file, header) || header.romHash != romHash_) continue;
        states_.push_back({item.path().string(), header.createdMs, static_cast<StateKind>(header.kind)});
    }
    std::sort(states_.begin(), states_.end(), [](const StateInfo& a, const StateInfo& b) {
        return a.createdMs != b.createdMs ? a.createdMs > b.createdMs : a.path > b.path;
    });
}

const StateInfo* SaveStates::latest(StateKind kind) const {
    for (const StateInfo& state : states_)
        if (state.kind == kind) return &state;
    return nullptr;
}

std::optional<StateInfo> SaveStates::save(GBA& gba, StateKind kind, std::string& error) {
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (ec) {
        error = "Couldn't create the states folder: " + ec.message();
        return std::nullopt;
    }
    // Files are named after the time they were made, so they sort by name too.
    s64 created = nowMs();
    fs::path path;
    do {
        path = fs::path(folder_) / (std::to_string(created) + kExtension);
        ++created;
    } while (fs::exists(path, ec));
    --created;
    if (!writeFile(path.string(), gba, kind, created, error)) return std::nullopt;
    refresh();
    prune(kind, kind == StateKind::Saved ? kMaxSaved : kMaxBackups);
    return StateInfo{path.string(), created, kind};
}

void SaveStates::prune(StateKind kind, int keep) {
    int seen = 0;
    bool removed = false;
    std::error_code ec;
    for (const StateInfo& state : states_) {  // newest first
        if (state.kind != kind || ++seen <= keep) continue;
        removed |= fs::remove(state.path, ec);
    }
    if (removed) refresh();
}

bool SaveStates::remove(const std::string& path) {
    std::error_code ec;
    const bool ok = fs::remove(path, ec);
    refresh();
    return ok;
}

bool SaveStates::writeFile(const std::string& path, GBA& gba, StateKind kind, s64 createdMs, std::string& error) {
    std::vector<u8> payload = gba.saveState();
    std::vector<u8> content(kScreenshotBytes);
    std::memcpy(content.data(), gba.ppu.framebuffer.data(), kScreenshotBytes);
    content.insert(content.end(), payload.begin(), payload.end());

    Header header;
    header.kind = static_cast<u32>(kind);
    header.romHash = gba.romHash();
    header.createdMs = createdMs;
    header.payloadSize = payload.size();
    header.contentHash = hashBytes(content.data(), content.size());
    std::vector<u8> bytes;
    StateStream out(bytes);
    serializeHeader(out, header);

    // Written to a temporary file and then swapped in, like the save files,
    // so a full disk or a crash can't leave a half-written state behind.
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (file) {
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            file.close();
        }
        if (!file) {
            std::remove(temp.c_str());
            error = "Couldn't write the state file (is the disk full?).";
            return false;
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        error = "Couldn't write the state file.";
        return false;
    }
    return true;
}

std::optional<StateFile> SaveStates::readFile(const std::string& path, const GBA& gba, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    Header header;
    if (!file || !readHeader(file, header)) {
        error = "This isn't a save state file.";
        return std::nullopt;
    }
    if (header.romHash != gba.romHash()) {
        error = "This state was made with a different game, or another version of it.";
        return std::nullopt;
    }
    if (header.stateVersion != GBA::kStateVersion) {
        error = "This state was made by an older version of the app and can't be loaded.";
        return std::nullopt;
    }
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || header.payloadSize > kMaxPayload || size != kHeaderSize + kScreenshotBytes + header.payloadSize) {
        error = "This state file is damaged.";
        return std::nullopt;
    }
    std::vector<u8> content(kScreenshotBytes + header.payloadSize);
    if (!file.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size())) ||
        hashBytes(content.data(), content.size()) != header.contentHash) {
        error = "This state file is damaged.";
        return std::nullopt;
    }
    StateFile result;
    result.info = {path, header.createdMs, static_cast<StateKind>(header.kind)};
    result.screenshot.resize(kScreenshotPixels);
    std::memcpy(result.screenshot.data(), content.data(), kScreenshotBytes);
    result.payload.assign(content.begin() + kScreenshotBytes, content.end());
    return result;
}

bool SaveStates::apply(GBA& gba, const StateFile& file) {
    if (file.screenshot.size() != kScreenshotPixels || !gba.loadState(file.payload)) return false;
    // The screen as it was, until the next frame draws over it.
    std::copy(file.screenshot.begin(), file.screenshot.end(), gba.ppu.framebuffer.begin());
    return true;
}

bool SaveStates::readScreenshot(const std::string& path, std::vector<u32>& pixels) {
    std::ifstream file(path, std::ios::binary);
    Header header;
    if (!file || !readHeader(file, header)) return false;
    pixels.resize(kScreenshotPixels);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(pixels.data()), kScreenshotBytes));
}
