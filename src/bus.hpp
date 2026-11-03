// bus.hpp - The memory map. Every CPU/DMA read or write goes through here.
//
//   0x00000000  BIOS (16 KiB)       0x05000000  Palette RAM (1 KiB)
//   0x02000000  EWRAM (256 KiB)     0x06000000  VRAM (96 KiB)
//   0x03000000  IWRAM (32 KiB)      0x07000000  OAM / sprite table (1 KiB)
//   0x04000000  I/O registers       0x08000000+ Cartridge ROM / save memory
//
// The bus also counts how many CPU cycles each access costs (wait states),
// which is how the emulator keeps all components in sync.
#pragma once

#include <array>
#include <vector>

#include "backup.hpp"
#include "common.hpp"

class GBA;

class Bus {
public:
    explicit Bus(GBA& gba);

    void reset();
    void loadRom(std::vector<u8> data);

    // Data accesses (count as non-sequential).
    u8  read8(u32 addr)  { cycles += waitN16_[(addr >> 24) & 15]; return read<u8>(addr); }
    u16 read16(u32 addr) { cycles += waitN16_[(addr >> 24) & 15]; return read<u16>(addr); }
    u32 read32(u32 addr) { cycles += waitN32_[(addr >> 24) & 15]; return read<u32>(addr); }
    void write8(u32 addr, u8 v)   { cycles += waitN16_[(addr >> 24) & 15]; write<u8>(addr, v); }
    void write16(u32 addr, u16 v) { cycles += waitN16_[(addr >> 24) & 15]; write<u16>(addr, v); }
    void write32(u32 addr, u32 v) { cycles += waitN32_[(addr >> 24) & 15]; write<u32>(addr, v); }

    // Instruction fetches. `sequential` is true when the CPU did not just branch.
    u16 fetch16(u32 addr, bool sequential);
    u32 fetch32(u32 addr, bool sequential);

    // Timing-free accessors for the HLE BIOS and debugging.
    template <typename T> T read(u32 addr);
    template <typename T> void write(u32 addr, T value);

    // I/O register file. `io` holds the last value written to each 16-bit
    // register; reads of "live" registers (VCOUNT, timers...) are computed.
    u16 ioRead16(u32 offset);
    void ioWrite16(u32 offset, u16 value);
    void ioWrite8(u32 offset, u8 value);
    u16& reg(u32 offset) { return io[offset >> 1]; }

    bool backupDirty() const;
    void clearBackupDirty();

    // Memory blocks. std::array has a fixed size known at compile time.
    std::array<u8, 0x4000> bios{};
    std::array<u8, 0x40000> ewram{};
    std::array<u8, 0x8000> iwram{};
    std::array<u8, 0x400> pram{};
    std::array<u8, 0x18000> vram{};
    std::array<u8, 0x400> oam{};
    std::array<u16, 0x200> io{};
    std::vector<u8> rom;

    SaveType saveType = SaveType::None;
    Eeprom eeprom;
    std::vector<u8> sram = std::vector<u8>(0x8000, 0xFF);
    bool sramDirty = false;

    u16 keyInput = 0x3FF;   // active-low: 1 = released
    u32 biosLatch = 0xE129F000;  // last opcode fetched from BIOS
    int cycles = 0;         // cycles consumed since the GBA last collected them

private:
    GBA& gba_;
    std::array<int, 16> waitN16_{}, waitS16_{}, waitN32_{}, waitS32_{};
    bool prefetch_ = false;

    void updateWaitStates();
    bool isEepromAddress(u32 addr) const;
    template <typename T> T openBus(u32 addr) const;
    static u32 vramOffset(u32 addr);
};
