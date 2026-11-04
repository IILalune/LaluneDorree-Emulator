// backup.hpp - Cartridge save memory.
//
// GBA carts store saves in one of several chip types. Many games use a
// serial EEPROM, which the game talks to one *bit* at a time through
// DMA channel 3. SRAM is also supported because it is trivial.
#pragma once

#include <vector>

#include "common.hpp"

class StateStream;

enum class SaveType { None, Eeprom, Sram };

class Eeprom {
public:
    void reset();

    // The game streams request bits in via 16-bit writes (only bit 0 counts)
    // and reads response bits back the same way.
    void writeBit(u16 value);
    u16 readBit();

    // EEPROMs come in 512-byte (6-bit addresses) or 8 KiB (14-bit addresses)
    // variants. The only way to tell is how many bits the game sends per DMA.
    void detectSizeFromDma(u32 transferCount);
    int addressBits() const { return addrBits_; }
    void setAddressBits(int bits) { addrBits_ = bits; }
    // Only the transfer in progress: `data` and the chip size belong to the
    // save file, which save states leave alone.
    void serialize(StateStream& s);

    std::vector<u8> data = std::vector<u8>(8192, 0xFF);
    bool dirty = false;

private:
    enum class State { Receiving, Sending };
    State state_ = State::Receiving;
    int addrBits_ = 0;  // 0 = not detected yet
    std::vector<u8> bits_;  // request bits received so far
    u64 sendBuffer_ = 0;
    int sendPos_ = 0;

    u32 bitsToNumber(std::size_t start, int count) const;
};
