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

    std::string gameTitle() const;
    std::string gameCode() const;

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
    void advance(int cycles);
};
