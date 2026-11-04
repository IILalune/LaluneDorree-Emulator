// gba.hpp - The whole console. Owns every hardware component and runs frames.
//
// C++ note: each component keeps a reference (`GBA&`) back to this object so
// it can talk to the others (e.g. the PPU raising an interrupt, the bus
// forwarding an I/O write to the timers). Members are constructed in the order
// they are declared, so references between them are safe once the
// constructor finishes.
#pragma once

#include <string>
#include <vector>

#include "apu.hpp"
#include "bios.hpp"
#include "bus.hpp"
#include "common.hpp"
#include "cpu.hpp"
#include "dma.hpp"
#include "ppu.hpp"
#include "timers.hpp"

class GBA {
public:
    GBA();

    // Load a ROM image (the raw contents of a .gba file).
    bool loadRom(std::vector<u8> data, std::string* error = nullptr);
    void reset();

    // Run until the PPU finishes a full frame (~280,896 CPU cycles).
    void runFrame();

    // `pressed` is a mask of Key values currently held down.
    void setKeys(u16 pressed);

    void raiseIrq(int irq);

    // Save-data helpers (battery backup inside the cartridge).
    bool loadSave(const std::string& path);
    bool writeSave(const std::string& path);
    bool saveDirty() const { return bus.backupDirty(); }

    // Save states: a snapshot of the whole console between two frames (see
    // state.hpp). The ROM, the cartridge's save memory and the screen image
    // are not included. loadState refuses data that doesn't fit this
    // program's layout and then leaves the console exactly as it was.
    static constexpr u32 kStateVersion = 1;  // bump when any serialize() changes
    std::vector<u8> saveState();
    bool loadState(const std::vector<u8>& data);

    std::string gameTitle() const;
    std::string gameCode() const;
    // Fingerprint of the loaded ROM, so a state is only loaded into the game
    // (and the exact version of it) that made it.
    u64 romHash() const { return romHash_; }

    // Components. Public so they can easily reach each other; in a bigger
    // program you might hide these behind accessors.
    Bus bus;
    CPU cpu;
    PPU ppu;
    APU apu;
    Timers timers;
    DMA dma;
    Bios bios;

private:
    u64 romHash_ = 0;

    void advance(int cycles);
    void serialize(StateStream& s);
};
