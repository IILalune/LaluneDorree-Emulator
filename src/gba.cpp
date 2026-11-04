#include "gba.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "state.hpp"

GBA::GBA()
    : bus(*this), cpu(*this), ppu(*this), apu(*this), timers(*this), dma(*this), bios(*this) {}

bool GBA::loadRom(std::vector<u8> data, std::string* error) {
    if (data.size() < 0xC0 || data.size() > 0x2000000) {
        if (error) *error = "File is not a valid GBA ROM (wrong size).";
        return false;
    }
    romHash_ = hashBytes(data.data(), data.size());
    bus.loadRom(std::move(data));
    reset();
    return true;
}

void GBA::serialize(StateStream& s) {
    bus.serialize(s);
    cpu.serialize(s);
    ppu.serialize(s);
    apu.serialize(s);
    timers.serialize(s);
    dma.serialize(s);
    bios.serialize(s);
    s.section("END ");
}

std::vector<u8> GBA::saveState() {
    std::vector<u8> data;
    data.reserve(512 * 1024);
    StateStream out(data);
    serialize(out);
    return data;
}

bool GBA::loadState(const std::vector<u8>& data) {
    // Loading writes fields one by one, so a state that turns out to be bad
    // halfway through would leave a mix of old and new. Keep a copy of the
    // current state to put back in that case.
    std::vector<u8> before = saveState();
    StateStream in(data.data(), data.size());
    serialize(in);
    if (in.ok() && in.atEnd()) return true;
    StateStream restore(before.data(), before.size());
    serialize(restore);
    return false;
}

void GBA::reset() {
    bus.reset();
    bios.install();
    cpu.reset();
    ppu.reset();
    apu.reset();
    timers.reset();
    dma.reset();

    // Register values the real BIOS leaves behind after the boot animation.
    bus.reg(0x020) = 0x100;  // BG2PA = 1.0
    bus.reg(0x026) = 0x100;  // BG2PD = 1.0
    bus.reg(0x030) = 0x100;  // BG3PA = 1.0
    bus.reg(0x036) = 0x100;  // BG3PD = 1.0
    bus.reg(0x088) = 0x200;  // SOUNDBIAS
    bus.reg(0x134) = 0x8000; // RCNT
    bus.reg(0x300) = 0x01;   // POSTFLG: "booted"
}

void GBA::raiseIrq(int irq) {
    bus.reg(0x202) |= static_cast<u16>(1u << irq);
}

void GBA::setKeys(u16 pressed) {
    bus.keyInput = static_cast<u16>(~pressed & 0x3FF);
    // Optional keypad interrupt when a chosen combination is held.
    const u16 keycnt = bus.reg(0x132);
    if (keycnt & 0x4000) {
        const u16 wanted = keycnt & 0x3FF;
        const bool all = keycnt & 0x8000;
        const bool hit = all ? (pressed & wanted) == wanted : (pressed & wanted) != 0;
        if (hit) raiseIrq(IrqKeypad);
    }
}

void GBA::advance(int cycles) {
    timers.tick(cycles);
    ppu.tick(cycles);
    apu.tick(cycles);
}

void GBA::runFrame() {
    ppu.frameReady = false;
    while (!ppu.frameReady) {
        if (cpu.halted) {
            // A halted CPU wakes as soon as any enabled interrupt is requested.
            if (bus.reg(0x200) & bus.reg(0x202) & 0x3FFF) {
                cpu.halted = false;
                continue;
            }
            // Nothing to do until the next hardware event: skip straight there.
            const int skip = std::max(1, std::min(ppu.cyclesUntilNextEvent(), timers.cyclesUntilOverflow()));
            advance(skip);
            continue;
        }
        cpu.step();
        const int cycles = bus.cycles;
        bus.cycles = 0;
        advance(cycles);
    }
}

std::string GBA::gameTitle() const {
    std::string title(reinterpret_cast<const char*>(bus.rom.data() + 0xA0), 12);
    title.erase(std::find(title.begin(), title.end(), '\0'), title.end());
    return title;
}

std::string GBA::gameCode() const {
    return std::string(reinterpret_cast<const char*>(bus.rom.data() + 0xAC), 4);
}

bool GBA::loadSave(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    // Saves are at most 32 KiB (SRAM); read one byte more so bigger files are
    // noticed (and rejected for EEPROM) without reading them into memory.
    std::vector<u8> data(bus.sram.size() + 1);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(file.gcount()));
    if (bus.saveType == SaveType::Eeprom) {
        if (data.size() == 512) bus.eeprom.setAddressBits(6);
        else if (data.size() == 8192) bus.eeprom.setAddressBits(14);
        else return false;
        std::copy(data.begin(), data.end(), bus.eeprom.data.begin());
        return true;
    }
    if (bus.saveType == SaveType::Sram) {
        std::copy_n(data.begin(), std::min(data.size(), bus.sram.size()), bus.sram.begin());
        return true;
    }
    return false;
}

bool GBA::writeSave(const std::string& path) {
    const u8* data = nullptr;
    std::size_t size = 0;
    if (bus.saveType == SaveType::Eeprom) {
        if (bus.eeprom.addressBits() == 0) return false;  // game never touched it
        data = bus.eeprom.data.data();
        size = bus.eeprom.addressBits() == 6 ? 512 : 8192;
    } else if (bus.saveType == SaveType::Sram) {
        data = bus.sram.data();
        size = bus.sram.size();
    } else {
        return false;
    }
    // Write a temporary file and then swap it in, so a crash or a full disk
    // halfway through leaves the previous save intact instead of a broken one.
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.close();
        if (!file) {
            std::remove(temp.c_str());
            return false;
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    bus.clearBackupDirty();
    return true;
}
