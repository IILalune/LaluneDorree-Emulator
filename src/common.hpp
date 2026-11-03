// common.hpp - Shared types and small helpers used by every part of the emulator.
//
// C++ note: fixed-width integer types (uint8_t etc.) matter a lot in emulation,
// because the GBA hardware works with exact 8/16/32-bit values and relies on
// wrap-around arithmetic. Short aliases keep the code readable.
#pragma once

#include <cstdint>
#include <cstring>

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// The GBA CPU runs at 2^24 Hz (~16.78 MHz).
constexpr u32 kCpuFrequency = 16777216;

constexpr int kScreenWidth  = 240;
constexpr int kScreenHeight = 160;

// Rotate a 32-bit value right. `constexpr` + `inline` lets the compiler fold
// this into a single ARM/x86 rotate instruction.
constexpr inline u32 ror32(u32 value, unsigned shift) {
    shift &= 31;
    return shift ? (value >> shift) | (value << (32 - shift)) : value;
}

// Read/write little-endian values from a byte buffer. memcpy is the portable,
// well-defined way to type-pun in C++ (casting pointers would be undefined
// behaviour because of alignment/strict-aliasing rules). Compilers turn it
// into a single load/store.
template <typename T>
inline T loadLE(const u8* data, u32 offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;  // Apple Silicon and x86 are both little-endian, like the GBA.
}

template <typename T>
inline void storeLE(u8* data, u32 offset, T value) {
    std::memcpy(data + offset, &value, sizeof(T));
}

// Interrupt sources, as bit numbers in the IE/IF registers.
enum Interrupt : int {
    IrqVBlank = 0,
    IrqHBlank = 1,
    IrqVCount = 2,
    IrqTimer0 = 3,
    IrqSerial = 7,
    IrqDma0   = 8,
    IrqKeypad = 12,
};

// GBA buttons, in KEYINPUT bit order.
enum Key : u16 {
    KeyA      = 1 << 0,
    KeyB      = 1 << 1,
    KeySelect = 1 << 2,
    KeyStart  = 1 << 3,
    KeyRight  = 1 << 4,
    KeyLeft   = 1 << 5,
    KeyUp     = 1 << 6,
    KeyDown   = 1 << 7,
    KeyR      = 1 << 8,
    KeyL      = 1 << 9,
};
