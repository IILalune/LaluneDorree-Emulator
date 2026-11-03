#include "bios.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

#include "gba.hpp"

namespace {
constexpr double kPi = 3.14159265358979323846;
}

Bios::Bios(GBA& gba) : gba_(gba) {}

void Bios::install() {
    auto& bios = gba_.bus.bios;
    bios.fill(0);
    auto put = [&](u32 addr, u32 opcode) { storeLE<u32>(bios.data(), addr, opcode); };

    put(0x00, 0xEAFFFFFE);  // reset vector: b . (never used, we boot directly)
    put(0x08, 0xE1B0F00E);  // SWI vector: movs pc, lr (SWIs are handled in C++)
    put(0x18, 0xEA000042);  // IRQ vector: b 0x128

    // The same interrupt dispatcher the real BIOS uses: save registers, call
    // the game's handler whose address is stored at 0x03FFFFFC, then return.
    put(0x128, 0xE92D500F);  // stmfd sp!, {r0-r3, r12, lr}
    put(0x12C, 0xE3A00301);  // mov   r0, #0x04000000
    put(0x130, 0xE28FE000);  // add   lr, pc, #0
    put(0x134, 0xE510F004);  // ldr   pc, [r0, #-4]
    put(0x138, 0xE8BD500F);  // ldmfd sp!, {r0-r3, r12, lr}
    put(0x13C, 0xE25EF004);  // subs  pc, lr, #4
    put(0x144, 0xE55EC002);  // (real BIOS opcode here; visible through the BIOS latch)
    waitingForInterrupt_ = false;
}

void Bios::swi(u32 number) {
    CPU& cpu = gba_.cpu;
    u32* r = cpu.r;
    gba_.bus.cycles += 10;  // rough cost of the BIOS call overhead
    // After a real SWI returns, reading the protected BIOS gives the last
    // opcode it fetched. Some games (and test ROMs) check this value.
    gba_.bus.biosLatch = 0xE3A02004;

    switch (number) {
    case 0x00: softReset(); break;
    case 0x01: registerRamReset(r[0]); break;
    case 0x02: cpu.halted = true; break;             // Halt
    case 0x03: cpu.halted = true; break;             // Stop (treated as halt)
    case 0x04: intrWait(r[0] != 0, static_cast<u16>(r[1])); break;
    case 0x05: intrWait(true, 1); break;             // VBlankIntrWait
    case 0x06: div(static_cast<s32>(r[0]), static_cast<s32>(r[1])); break;
    case 0x07: div(static_cast<s32>(r[1]), static_cast<s32>(r[0])); break;  // DivArm
    case 0x08: sqrt(); break;
    case 0x09: arcTan(); break;
    case 0x0A: arcTan2(); break;
    case 0x0B: cpuSet(); break;
    case 0x0C: cpuFastSet(); break;
    case 0x0D: r[0] = 0xBAAE187F; break;             // GetBiosChecksum
    case 0x0E: bgAffineSet(); break;
    case 0x0F: objAffineSet(); break;
    case 0x10: bitUnPack(); break;
    case 0x11: lz77(false); break;
    case 0x12: lz77(true); break;
    case 0x13: huffman(); break;
    case 0x14: runLength(false); break;
    case 0x15: runLength(true); break;
    case 0x16: diffUnFilter(1, false); break;
    case 0x17: diffUnFilter(1, true); break;
    case 0x18: diffUnFilter(2, true); break;
    case 0x19: break;                                // SoundBias: no audible effect here
    case 0x1F: midiKeyToFreq(); break;
    default: {
        static std::set<u32> reported;
        if (reported.insert(number).second) {
            std::fprintf(stderr, "[bios] unimplemented SWI 0x%02X\n", number);
        }
        break;
    }
    }
}

void Bios::softReset() {
    Bus& bus = gba_.bus;
    const bool toRam = bus.read<u8>(0x03007FFA) != 0;
    for (u32 a = 0x7E00; a < 0x8000; ++a) bus.iwram[a] = 0;
    gba_.cpu.reset();
    gba_.cpu.jumpTo(toRam ? 0x02000000 : 0x08000000);
}

void Bios::registerRamReset(u32 flags) {
    Bus& bus = gba_.bus;
    if (flags & 0x01) bus.ewram.fill(0);
    if (flags & 0x02) std::fill(bus.iwram.begin(), bus.iwram.begin() + 0x7E00, 0);
    if (flags & 0x04) bus.pram.fill(0);
    if (flags & 0x08) bus.vram.fill(0);
    if (flags & 0x10) bus.oam.fill(0);
    if (flags & 0x80) for (u32 off = 0x60; off < 0xA8; off += 2) bus.ioWrite16(off, 0);
}

void Bios::intrWait(bool discardOld, u16 mask) {
    // We can't block inside a C++ function, so instead we halt the CPU and
    // rewind the PC onto this SWI. After the next interrupt has been handled
    // the SWI runs again and checks whether the interrupt we want happened.
    Bus& bus = gba_.bus;
    constexpr u32 kBiosIf = 0x03007FF8;  // the game's handler ORs flags in here

    if (!waitingForInterrupt_) {
        waitingForInterrupt_ = true;
        if (discardOld) bus.write<u16>(kBiosIf, bus.read<u16>(kBiosIf) & ~mask);
    }
    bus.reg(0x208) = 1;  // IME on

    const u16 flags = bus.read<u16>(kBiosIf);
    if (flags & mask) {
        bus.write<u16>(kBiosIf, flags & ~mask);
        waitingForInterrupt_ = false;
        return;
    }
    gba_.cpu.halted = true;
    gba_.cpu.jumpTo(gba_.cpu.currentInstruction());
}

void Bios::div(s32 numerator, s32 denominator) {
    u32* r = gba_.cpu.r;
    gba_.bus.cycles += 30;
    if (denominator == 0) {  // the real BIOS loops forever; return something sane
        r[0] = numerator < 0 ? static_cast<u32>(-1) : 1;
        r[1] = static_cast<u32>(numerator);
        r[3] = 1;
        return;
    }
    if (numerator == INT32_MIN && denominator == -1) {
        r[0] = 0x80000000; r[1] = 0; r[3] = 0x80000000;
        return;
    }
    const s32 quotient = numerator / denominator;
    r[0] = static_cast<u32>(quotient);
    r[1] = static_cast<u32>(numerator % denominator);
    r[3] = static_cast<u32>(quotient < 0 ? -quotient : quotient);
}

void Bios::sqrt() {
    u32* r = gba_.cpu.r;
    r[0] = static_cast<u32>(std::sqrt(static_cast<double>(r[0])));
}

void Bios::arcTan() {
    // Polynomial approximation that matches the hardware's results.
    u32* r = gba_.cpu.r;
    const s32 x = static_cast<s32>(r[0]);
    const s32 a = -((x * x) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    r[0] = static_cast<u32>((x * b) >> 16);
}

void Bios::arcTan2() {
    // Returns the angle of vector (x, y) with 0x10000 meaning a full circle.
    u32* r = gba_.cpu.r;
    const double x = static_cast<s16>(r[0]);
    const double y = static_cast<s16>(r[1]);
    double angle = std::atan2(y, x);
    if (angle < 0) angle += 2 * kPi;
    r[0] = static_cast<u32>(angle / (2 * kPi) * 65536.0) & 0xFFFF;
}

void Bios::cpuSet() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    const u32 control = r[2];
    const u32 count = control & 0x1FFFFF;
    const bool fill = control & (1 << 24);
    if (control & (1 << 26)) {  // 32-bit units
        src &= ~3u; dst &= ~3u;
        for (u32 i = 0; i < count; ++i) {
            bus.write32(dst, bus.read32(src));
            dst += 4;
            if (!fill) src += 4;
        }
    } else {
        src &= ~1u; dst &= ~1u;
        for (u32 i = 0; i < count; ++i) {
            bus.write16(dst, bus.read16(src));
            dst += 2;
            if (!fill) src += 2;
        }
    }
}

void Bios::cpuFastSet() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0] & ~3u, dst = r[1] & ~3u;
    const u32 control = r[2];
    const u32 count = ((control & 0x1FFFFF) + 7) & ~7u;  // always multiples of 8 words
    const bool fill = control & (1 << 24);
    const u32 fillValue = bus.read32(src);
    for (u32 i = 0; i < count; ++i) {
        bus.write32(dst, fill ? fillValue : bus.read32(src));
        dst += 4;
        if (!fill) src += 4;
    }
}

void Bios::bgAffineSet() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    for (u32 i = 0; i < r[2]; ++i) {
        const double ox = static_cast<s32>(bus.read<u32>(src)) / 256.0;
        const double oy = static_cast<s32>(bus.read<u32>(src + 4)) / 256.0;
        const double cx = static_cast<s16>(bus.read<u16>(src + 8));
        const double cy = static_cast<s16>(bus.read<u16>(src + 10));
        const double sx = static_cast<s16>(bus.read<u16>(src + 12)) / 256.0;
        const double sy = static_cast<s16>(bus.read<u16>(src + 14)) / 256.0;
        const double theta = (bus.read<u16>(src + 16) >> 8) / 128.0 * kPi;
        src += 20;

        const double pa = std::cos(theta) * sx, pb = -std::sin(theta) * sx;
        const double pc = std::sin(theta) * sy, pd = std::cos(theta) * sy;
        const double startX = ox - (pa * cx + pb * cy);
        const double startY = oy - (pc * cx + pd * cy);

        bus.write<u16>(dst + 0, static_cast<u16>(static_cast<s16>(pa * 256)));
        bus.write<u16>(dst + 2, static_cast<u16>(static_cast<s16>(pb * 256)));
        bus.write<u16>(dst + 4, static_cast<u16>(static_cast<s16>(pc * 256)));
        bus.write<u16>(dst + 6, static_cast<u16>(static_cast<s16>(pd * 256)));
        bus.write<u32>(dst + 8, static_cast<u32>(static_cast<s32>(startX * 256)));
        bus.write<u32>(dst + 12, static_cast<u32>(static_cast<s32>(startY * 256)));
        dst += 16;
    }
}

void Bios::objAffineSet() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    const u32 stride = r[3];  // 2 = packed array, 8 = straight into OAM
    for (u32 i = 0; i < r[2]; ++i) {
        const double sx = static_cast<s16>(bus.read<u16>(src)) / 256.0;
        const double sy = static_cast<s16>(bus.read<u16>(src + 2)) / 256.0;
        const double theta = (bus.read<u16>(src + 4) >> 8) / 128.0 * kPi;
        src += 8;
        const double values[4] = {std::cos(theta) * sx, -std::sin(theta) * sx,
                                  std::sin(theta) * sy, std::cos(theta) * sy};
        for (double v : values) {
            bus.write<u16>(dst, static_cast<u16>(static_cast<s16>(v * 256)));
            dst += stride;
        }
    }
}

void Bios::bitUnPack() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    const u32 info = r[2];
    const u32 length = bus.read<u16>(info);
    const u32 srcWidth = bus.read<u8>(info + 2);
    const u32 dstWidth = bus.read<u8>(info + 3);
    u32 offset = bus.read<u32>(info + 4);
    const bool offsetZeros = offset & 0x80000000;
    offset &= 0x7FFFFFFF;
    if (srcWidth == 0 || dstWidth == 0) return;

    u32 out = 0, outBits = 0;
    const u32 mask = (1u << srcWidth) - 1;
    for (u32 i = 0; i < length; ++i) {
        const u8 byte = bus.read<u8>(src++);
        for (u32 bit = 0; bit < 8; bit += srcWidth) {
            u32 value = (byte >> bit) & mask;
            if (value || offsetZeros) value += offset;
            out |= value << outBits;
            outBits += dstWidth;
            if (outBits >= 32) {
                bus.write<u32>(dst, out);
                dst += 4;
                out = 0;
                outBits = 0;
            }
        }
    }
}

void Bios::writeOutput(u32& dst, const u8* data, std::size_t size, bool vram) {
    Bus& bus = gba_.bus;
    if (vram) {  // VRAM can't take byte writes, so output 16 bits at a time
        for (std::size_t i = 0; i < size; i += 2) {
            const u16 lo = data[i];
            const u16 hi = (i + 1 < size) ? data[i + 1] : 0;
            bus.write<u16>(dst, static_cast<u16>(lo | (hi << 8)));
            dst += 2;
        }
    } else {
        for (std::size_t i = 0; i < size; ++i) bus.write<u8>(dst++, data[i]);
    }
    bus.cycles += static_cast<int>(size) * 4;  // decompression is not free
}

void Bios::lz77(bool vram) {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    const u32 size = bus.read<u32>(src) >> 8;
    src += 4;

    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        u8 flags = bus.read<u8>(src++);
        for (int i = 0; i < 8 && out.size() < size; ++i, flags <<= 1) {
            if (flags & 0x80) {  // back-reference: copy `length` bytes from earlier output
                const u8 b1 = bus.read<u8>(src++);
                const u8 b2 = bus.read<u8>(src++);
                const u32 length = (b1 >> 4) + 3;
                const u32 distance = (((b1 & 0xF) << 8) | b2) + 1;
                for (u32 j = 0; j < length && out.size() < size; ++j) {
                    out.push_back(distance <= out.size() ? out[out.size() - distance] : 0);
                }
            } else {  // literal byte
                out.push_back(bus.read<u8>(src++));
            }
        }
    }
    writeOutput(dst, out.data(), out.size(), vram);
}

void Bios::huffman() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    const u32 src = r[0] & ~3u;
    u32 dst = r[1];
    const u32 header = bus.read<u32>(src);
    const u32 size = header >> 8;
    u32 bits = header & 0xF;
    if (bits != 4 && bits != 8) bits = 8;

    const u32 treeSize = bus.read<u8>(src + 4);
    const u32 root = src + 5;
    u32 dataAddr = src + 4 + (treeSize + 1) * 2;
    u32 node = root;
    u32 outWord = 0, outBits = 0, written = 0;

    while (written < size) {
        const u32 word = bus.read<u32>(dataAddr);
        dataAddr += 4;
        for (int bit = 31; bit >= 0 && written < size; --bit) {
            const u8 value = bus.read<u8>(node);
            const u32 next = (node & ~1u) + (value & 0x3F) * 2 + 2;
            const bool right = (word >> bit) & 1;
            const u32 child = next + (right ? 1 : 0);
            const bool leaf = right ? (value & 0x40) : (value & 0x80);
            if (leaf) {
                outWord |= (bus.read<u8>(child) & ((1u << bits) - 1)) << outBits;
                outBits += bits;
                node = root;
                if (outBits == 32) {
                    bus.write<u32>(dst, outWord);
                    dst += 4;
                    written += 4;
                    outWord = 0;
                    outBits = 0;
                }
            } else {
                node = child;
            }
        }
    }
}

void Bios::runLength(bool vram) {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    const u32 size = bus.read<u32>(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        const u8 flag = bus.read<u8>(src++);
        if (flag & 0x80) {  // a run of one repeated byte
            const u32 length = (flag & 0x7F) + 3;
            const u8 value = bus.read<u8>(src++);
            for (u32 i = 0; i < length && out.size() < size; ++i) out.push_back(value);
        } else {            // uncompressed bytes
            const u32 length = (flag & 0x7F) + 1;
            for (u32 i = 0; i < length && out.size() < size; ++i) out.push_back(bus.read<u8>(src++));
        }
    }
    writeOutput(dst, out.data(), out.size(), vram);
}

void Bios::diffUnFilter(int unitBytes, bool vram) {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    u32 src = r[0], dst = r[1];
    const u32 size = bus.read<u32>(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    if (unitBytes == 1) {
        u8 value = 0;
        for (u32 i = 0; i < size; ++i) {
            value = static_cast<u8>(value + bus.read<u8>(src++));
            out.push_back(value);
        }
    } else {
        u16 value = 0;
        for (u32 i = 0; i < size; i += 2) {
            value = static_cast<u16>(value + bus.read<u16>(src));
            src += 2;
            out.push_back(static_cast<u8>(value));
            out.push_back(static_cast<u8>(value >> 8));
        }
    }
    writeOutput(dst, out.data(), out.size(), vram);
}

void Bios::midiKeyToFreq() {
    Bus& bus = gba_.bus;
    u32* r = gba_.cpu.r;
    const double baseFreq = bus.read<u32>(r[0] + 4);
    const double exponent = (180.0 - r[1] - r[2] / 256.0) / 12.0;
    r[0] = static_cast<u32>(baseFreq / std::pow(2.0, exponent));
}
