#include "bus.hpp"

#include <type_traits>

#include "gba.hpp"
#include "state.hpp"

Bus::Bus(GBA& gba) : gba_(gba) {}

void Bus::reset() {
    ewram.fill(0);
    iwram.fill(0);
    pram.fill(0);
    vram.fill(0);
    oam.fill(0);
    io.fill(0);
    keyInput = 0x3FF;
    biosLatch = 0xE129F000;
    cycles = 0;
    eeprom.reset();
    updateWaitStates();
}

void Bus::loadRom(std::vector<u8> data) {
    rom = std::move(data);  // std::move hands over the buffer without copying 16 MiB

    // Games built with Nintendo's SDK embed a library ID string that tells
    // us which save chip the cartridge has.
    auto contains = [this](const char* needle) {
        const std::size_t len = std::strlen(needle);
        for (std::size_t i = 0; i + len <= rom.size(); i += 4) {
            if (std::memcmp(rom.data() + i, needle, len) == 0) return true;
        }
        return false;
    };
    if (contains("EEPROM_V")) saveType = SaveType::Eeprom;
    else if (contains("SRAM_V") || contains("SRAM_F_V")) saveType = SaveType::Sram;
    else saveType = SaveType::None;
}

bool Bus::backupDirty() const { return eeprom.dirty || sramDirty; }
void Bus::clearBackupDirty() { eeprom.dirty = false; sramDirty = false; }

// ---------------------------------------------------------------------------
// Wait states
// ---------------------------------------------------------------------------

void Bus::updateWaitStates() {
    const u16 waitcnt = reg(0x204);
    static constexpr int kNonSeq[4] = {4, 3, 2, 8};

    // Fast internal memory first: {N16, S16, N32, S32} (1 = no wait states).
    for (int region = 0; region < 16; ++region) {
        waitN16_[region] = waitS16_[region] = 1;
        waitN32_[region] = waitS32_[region] = 1;
    }
    waitN16_[2] = waitS16_[2] = 3;  // EWRAM has a 16-bit bus and 2 wait states
    waitN32_[2] = waitS32_[2] = 6;
    for (int region : {5, 6}) {     // palette/VRAM: 16-bit bus
        waitN32_[region] = waitS32_[region] = 2;
    }

    // Cartridge ROM has three configurable "wait state" mirrors.
    const int n[3] = {kNonSeq[(waitcnt >> 2) & 3], kNonSeq[(waitcnt >> 5) & 3], kNonSeq[(waitcnt >> 8) & 3]};
    const int s[3] = {(waitcnt & 0x10) ? 1 : 2, (waitcnt & 0x80) ? 1 : 4, (waitcnt & 0x400) ? 1 : 8};
    for (int ws = 0; ws < 3; ++ws) {
        for (int region : {8 + ws * 2, 9 + ws * 2}) {
            waitN16_[region] = 1 + n[ws];
            waitS16_[region] = 1 + s[ws];
            waitN32_[region] = waitN16_[region] + waitS16_[region];
            waitS32_[region] = 2 * waitS16_[region];
        }
    }
    const int sramWait = 1 + kNonSeq[waitcnt & 3];
    for (int region : {14, 15}) {
        waitN16_[region] = waitS16_[region] = waitN32_[region] = waitS32_[region] = sramWait;
    }
    prefetch_ = waitcnt & 0x4000;
}

u16 Bus::fetch16(u32 addr, bool sequential) {
    const u32 region = (addr >> 24) & 15;
    if (prefetch_ && sequential && region >= 8 && region <= 13) cycles += 1;
    else cycles += sequential ? waitS16_[region] : waitN16_[region];
    const u16 op = read<u16>(addr);
    if (region == 0) biosLatch = loadLE<u32>(bios.data(), (addr + 4) & 0x3FFC);
    return op;
}

u32 Bus::fetch32(u32 addr, bool sequential) {
    const u32 region = (addr >> 24) & 15;
    if (prefetch_ && sequential && region >= 8 && region <= 13) cycles += 2;
    else cycles += sequential ? waitS32_[region] : waitN32_[region];
    const u32 op = read<u32>(addr);
    // The pipeline has already fetched two instructions further ahead.
    if (region == 0) biosLatch = loadLE<u32>(bios.data(), (addr + 8) & 0x3FFC);
    return op;
}

// ---------------------------------------------------------------------------
// Raw reads/writes
// ---------------------------------------------------------------------------

u32 Bus::vramOffset(u32 addr) {
    // VRAM is 96 KiB but mirrored in 128 KiB steps; the last 32 KiB of each
    // 128 KiB block mirror the sprite area at 0x10000.
    u32 offset = addr & 0x1FFFF;
    if (offset >= 0x18000) offset -= 0x8000;
    return offset;
}

bool Bus::isEepromAddress(u32 addr) const {
    if (saveType != SaveType::Eeprom) return false;
    if ((addr >> 24) != 0x0D) return false;
    // Carts larger than 16 MiB only expose the EEPROM in the last 256 bytes.
    return rom.size() <= 0x1000000 || (addr & 0x1FFFFFF) >= 0x1FFFF00;
}

template <typename T>
T Bus::openBus(u32 addr) const {
    // Reading unmapped memory returns whatever was last on the bus, which is
    // usually the most recently fetched instruction.
    return static_cast<T>(gba_.cpu.openBus >> ((addr & 3) * 8));
}

template <typename T>
T Bus::read(u32 addr) {
    // SRAM has an 8-bit bus: wider reads see the same byte repeated.
    if ((addr >> 24) == 0x0E || (addr >> 24) == 0x0F) {
        if (saveType != SaveType::Sram) return static_cast<T>(0xFFFFFFFF);
        const u8 byte = sram[addr & 0x7FFF];
        return static_cast<T>(byte * 0x01010101u);
    }

    addr &= ~static_cast<u32>(sizeof(T) - 1);  // the bus forces alignment
    switch (addr >> 24) {
    case 0x00:
        if (addr < 0x4000) {
            // The BIOS can only be read while executing inside it.
            if (gba_.cpu.r[15] < 0x4000 + 8) return loadLE<T>(bios.data(), addr);
            return static_cast<T>(biosLatch >> ((addr & 3) * 8));
        }
        return openBus<T>(addr);
    case 0x02: return loadLE<T>(ewram.data(), addr & 0x3FFFF);
    case 0x03: return loadLE<T>(iwram.data(), addr & 0x7FFF);
    case 0x04: {
        if ((addr & 0xFFFFFF) >= 0x400) return openBus<T>(addr);
        const u32 off = addr & 0x3FF;
        if constexpr (sizeof(T) == 1) {
            return static_cast<T>(ioRead16(off & ~1u) >> ((off & 1) * 8));
        } else if constexpr (sizeof(T) == 2) {
            return ioRead16(off);
        } else {
            return ioRead16(off) | (static_cast<u32>(ioRead16(off + 2)) << 16);
        }
    }
    case 0x05: return loadLE<T>(pram.data(), addr & 0x3FF);
    case 0x06: return loadLE<T>(vram.data(), vramOffset(addr));
    case 0x07: return loadLE<T>(oam.data(), addr & 0x3FF);
    case 0x0D:
        if (isEepromAddress(addr)) return static_cast<T>(eeprom.readBit());
        [[fallthrough]];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: {
        const u32 offset = addr & 0x1FFFFFF;
        if (offset + sizeof(T) <= rom.size()) return loadLE<T>(rom.data(), offset);
        // Beyond the end of the ROM the cartridge bus returns address bits.
        if constexpr (sizeof(T) == 4) {
            return ((offset >> 1) & 0xFFFF) | ((((offset + 2) >> 1) & 0xFFFF) << 16);
        } else {
            return static_cast<T>((offset >> 1) >> ((addr & 1) * 8));
        }
    }
    default: return openBus<T>(addr);
    }
}

template <typename T>
void Bus::write(u32 addr, T value) {
    if ((addr >> 24) == 0x0E || (addr >> 24) == 0x0F) {
        if (saveType == SaveType::Sram) {
            sram[addr & 0x7FFF] = static_cast<u8>(value >> ((addr & (sizeof(T) - 1)) * 8));
            sramDirty = true;
        }
        return;
    }

    addr &= ~static_cast<u32>(sizeof(T) - 1);
    switch (addr >> 24) {
    case 0x02: storeLE<T>(ewram.data(), addr & 0x3FFFF, value); break;
    case 0x03: storeLE<T>(iwram.data(), addr & 0x7FFF, value); break;
    case 0x04: {
        if ((addr & 0xFFFFFF) >= 0x400) break;
        const u32 off = addr & 0x3FF;
        if constexpr (sizeof(T) == 1) {
            ioWrite8(off, value);
        } else if constexpr (sizeof(T) == 2) {
            ioWrite16(off, value);
        } else {
            ioWrite16(off, static_cast<u16>(value));
            ioWrite16(off + 2, static_cast<u16>(value >> 16));
        }
        break;
    }
    case 0x05:
        // Byte writes to 16-bit video memory store the byte in both halves.
        if constexpr (sizeof(T) == 1) storeLE<u16>(pram.data(), addr & 0x3FE, value * 0x101);
        else storeLE<T>(pram.data(), addr & 0x3FF, value);
        break;
    case 0x06: {
        const u32 offset = vramOffset(addr);
        if constexpr (sizeof(T) == 1) {
            const bool bitmapMode = (reg(0) & 7) >= 3;
            if (offset < (bitmapMode ? 0x14000u : 0x10000u)) {
                storeLE<u16>(vram.data(), offset & ~1u, static_cast<u16>(value * 0x101));
            }
            // Byte writes to sprite VRAM are ignored by the hardware.
        } else {
            storeLE<T>(vram.data(), offset, value);
        }
        break;
    }
    case 0x07:
        if constexpr (sizeof(T) != 1) storeLE<T>(oam.data(), addr & 0x3FF, value);
        break;
    case 0x0D:
        if (isEepromAddress(addr)) eeprom.writeBit(static_cast<u16>(value));
        break;
    default: break;  // ROM and unmapped areas are read-only
    }
}

// Explicit instantiations: the template bodies live in this .cpp file, so we
// tell the compiler which versions to generate for other files to link to.
template u8 Bus::read<u8>(u32);
template u16 Bus::read<u16>(u32);
template u32 Bus::read<u32>(u32);
template void Bus::write<u8>(u32, u8);
template void Bus::write<u16>(u32, u16);
template void Bus::write<u32>(u32, u32);

// ---------------------------------------------------------------------------
// I/O registers
// ---------------------------------------------------------------------------

u16 Bus::ioRead16(u32 off) {
    off &= 0x3FE;
    switch (off) {
    case 0x006: return gba_.ppu.vcount();
    case 0x010: case 0x012: case 0x014: case 0x016: case 0x018: case 0x01A: case 0x01C: case 0x01E:
        return 0;  // BG scroll registers are write-only
    case 0x084: return (reg(0x84) & 0x80) | gba_.apu.channelStatus();
    case 0x100: case 0x104: case 0x108: case 0x10C:
        return gba_.timers.readCounter((off - 0x100) / 4);
    case 0x130: return keyInput;
    default: break;
    }
    if (off >= 0x20 && off < 0x40) return 0;    // affine parameters (write-only)
    if (off >= 0x90 && off < 0xA0) return gba_.apu.readWaveRam(off - 0x90);
    if (off >= 0xA0 && off < 0xB0) return 0;    // sound FIFOs (write-only)
    if (off >= 0xB0 && off < 0xE0) {            // DMA: only control is readable
        const u32 rel = (off - 0xB0) % 12;
        return rel == 10 ? reg(off) : 0;
    }
    return reg(off);
}

void Bus::ioWrite8(u32 off, u8 value) {
    switch (off) {
    case 0x202: reg(0x202) &= ~value; return;          // IF: write 1 to acknowledge
    case 0x203: reg(0x202) &= ~(value << 8); return;
    case 0x300: reg(0x300) = (reg(0x300) & 0xFF00) | value; return;  // POSTFLG
    case 0x301: gba_.cpu.halted = true; return;         // HALTCNT
    default: break;
    }
    if (off >= 0xA0 && off < 0xA8) {                    // sound FIFO byte write
        gba_.apu.writeFifo(off < 0xA4 ? 0 : 1, value);
        return;
    }
    // Everything else: merge with the other half and do a 16-bit write.
    const u16 current = reg(off & ~1u);
    const u16 merged = (off & 1) ? static_cast<u16>((current & 0x00FF) | (value << 8))
                                 : static_cast<u16>((current & 0xFF00) | value);
    ioWrite16(off & ~1u, merged);
}

void Bus::ioWrite16(u32 off, u16 value) {
    off &= 0x3FE;
    switch (off) {
    case 0x004:  // DISPSTAT: bits 0-2 are status flags owned by the PPU
        reg(off) = (reg(off) & 0x7) | (value & 0xFFF8);
        return;
    case 0x006: return;  // VCOUNT is read-only
    case 0x028: case 0x02A: case 0x02C: case 0x02E:
        reg(off) = value;
        gba_.ppu.latchAffine(0);
        return;
    case 0x038: case 0x03A: case 0x03C: case 0x03E:
        reg(off) = value;
        gba_.ppu.latchAffine(1);
        return;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        gba_.timers.writeReload((off - 0x100) / 4, value);
        return;
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        gba_.timers.writeControl((off - 0x102) / 4, value);
        reg(off) = value;
        return;
    case 0x130: return;  // KEYINPUT is read-only
    case 0x132:
        reg(off) = value;
        gba_.setKeys(static_cast<u16>(~keyInput & 0x3FF));  // re-check keypad IRQ
        return;
    case 0x202: reg(off) &= ~value; return;
    case 0x204:
        reg(off) = value & 0x7FFF;
        updateWaitStates();
        return;
    case 0x208: reg(off) = value & 1; return;
    case 0x300:
        // A 16-bit write here also writes HALTCNT (the high byte), which halts.
        reg(off) = value & 0xFF;
        gba_.cpu.halted = true;
        return;
    default: break;
    }

    if (off >= 0x60 && off < 0xA0) {
        if (off >= 0x90) { gba_.apu.writeWaveRam(off - 0x90, value); return; }
        reg(off) = value;
        gba_.apu.writeRegister(off, value);
        return;
    }
    if (off >= 0xA0 && off < 0xA8) {
        const int fifo = off < 0xA4 ? 0 : 1;
        gba_.apu.writeFifo(fifo, static_cast<u8>(value));
        gba_.apu.writeFifo(fifo, static_cast<u8>(value >> 8));
        return;
    }
    if (off >= 0xB0 && off < 0xE0) {
        const u16 old = reg(off);
        reg(off) = value;
        const u32 rel = (off - 0xB0) % 12;
        if (rel == 10) gba_.dma.writeControl(static_cast<int>((off - 0xB0) / 12), old, value);
        return;
    }
    reg(off) = value;
}

void Bus::serialize(StateStream& s) {
    s.section("BUS ");
    s.value(ewram);
    s.value(iwram);
    s.value(pram);
    s.value(vram);
    s.value(oam);
    s.value(io);
    s.value(keyInput);
    s.value(biosLatch);
    s.value(cycles);
    eeprom.serialize(s);
    // The BIOS image is rewritten identically by Bios::install, and the wait
    // states follow from WAITCNT, so neither needs storing.
    if (s.loading()) updateWaitStates();
}
