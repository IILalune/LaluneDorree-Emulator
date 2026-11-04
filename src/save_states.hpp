// save_states.hpp - Save state files: the snapshots on the Save States screen.
//
// Each game has its own folder of states (States/<game>/ next to the saves).
// You can keep many states and go back to any of them, newest first. Loading
// one first keeps the moment you were at as a "backup" state, so a load can
// always be undone.
//
// A state file holds one snapshot of the console plus its screenshot:
//
//   header      56 bytes: "LLDSTATE", file and state versions, kind, ROM
//               fingerprint, creation time, payload size and a hash of
//               everything after the header
//   screenshot  240x160 pixels, 0xAARRGGBB (also the screen after loading)
//   payload     GBA::saveState()
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"

class GBA;

enum class StateKind : u32 {
    Saved = 0,   // made by you (Save States screen or the quick-save key)
    Backup = 1,  // made automatically just before loading another state
};

struct StateInfo {
    std::string path;
    s64 createdMs = 0;  // when it was made: milliseconds since 1970 (UTC)
    StateKind kind = StateKind::Saved;
};

// A state file read into memory and checked against the running game.
struct StateFile {
    StateInfo info;
    std::vector<u32> screenshot;
    std::vector<u8> payload;
};

class SaveStates {
public:
    // The oldest are deleted beyond these, separately for each kind.
    static constexpr int kMaxSaved = 30;
    static constexpr int kMaxBackups = 5;

    // Switches to a game's folder (created when its first state is saved).
    // Only states made with the ROM that has fingerprint `romHash` are listed.
    void open(std::string folder, u64 romHash);
    void refresh();
    const std::vector<StateInfo>& list() const { return states_; }  // newest first
    const StateInfo* latest(StateKind kind) const;

    // Saves the console as it is now. Returns the new state, or nothing and
    // sets `error`.
    std::optional<StateInfo> save(GBA& gba, StateKind kind, std::string& error);
    bool remove(const std::string& path);

    // ---- Single files (also used by headless mode) ----

    static bool writeFile(const std::string& path, GBA& gba, StateKind kind, s64 createdMs, std::string& error);
    // Reads a state and checks that it is undamaged and was made by this
    // version of the app for the game loaded in `gba`.
    static std::optional<StateFile> readFile(const std::string& path, const GBA& gba, std::string& error);
    // Puts a state read by readFile into the console. False (console
    // unchanged) if its contents don't fit, which readFile normally rules out.
    static bool apply(GBA& gba, const StateFile& file);
    // Just the screenshot, for the list. False if the file isn't a state.
    static bool readScreenshot(const std::string& path, std::vector<u32>& pixels);

    static s64 nowMs();

private:
    std::string folder_;
    u64 romHash_ = 0;
    std::vector<StateInfo> states_;

    void prune(StateKind kind, int keep);
};
