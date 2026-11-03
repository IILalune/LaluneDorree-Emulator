// cpu_thumb.cpp - The 16-bit Thumb instruction set.
//
// Thumb is a compressed encoding of the most common ARM operations. Most GBA
// game code is Thumb because the cartridge bus is only 16 bits wide, so 16-bit
// instructions can be fetched in a single access.
#include <bit>

#include "cpu.hpp"
#include "gba.hpp"

void CPU::executeThumb(u16 op) {
    Bus& bus = gba_.bus;
    switch (op >> 13) {
    case 0: {
        const int rs = (op >> 3) & 7, rd = op & 7;
        if (((op >> 11) & 3) == 3) {  // ADD/SUB register or 3-bit immediate
            const u32 operand = (op & 0x400) ? ((op >> 6) & 7) : r[(op >> 6) & 7];
            r[rd] = (op & 0x200) ? sub(r[rs], operand, 1, true) : add(r[rs], operand, 0, true);
        } else {  // LSL/LSR/ASR by immediate
            bool carry = flagC();
            r[rd] = shiftByImmediate((op >> 11) & 3, r[rs], (op >> 6) & 31, carry);
            setNZ(r[rd]);
            setFlag(kFlagC, carry);
        }
        return;
    }
    case 1: {  // MOV/CMP/ADD/SUB with 8-bit immediate
        const int rd = (op >> 8) & 7;
        const u32 imm = op & 0xFF;
        switch ((op >> 11) & 3) {
        case 0: r[rd] = imm; setNZ(imm); break;
        case 1: sub(r[rd], imm, 1, true); break;
        case 2: r[rd] = add(r[rd], imm, 0, true); break;
        case 3: r[rd] = sub(r[rd], imm, 1, true); break;
        }
        return;
    }
    case 2:
        if ((op >> 10) == 0x10) return thumbAlu(op);
        if ((op >> 10) == 0x11) return thumbHiRegister(op);
        if ((op >> 11) == 0x09) {  // LDR Rd, [PC, #imm]
            const u32 addr = (r[15] & ~2u) + (op & 0xFF) * 4;
            r[(op >> 8) & 7] = bus.read32(addr);
            idle(1);
            return;
        }
        return thumbLoadStore(op);
    case 3: case 4:
        return thumbLoadStore(op);
    case 5:
        if (!(op & 0x1000)) {  // ADD Rd, PC/SP, #imm
            const u32 base = (op & 0x800) ? r[13] : (r[15] & ~2u);
            r[(op >> 8) & 7] = base + (op & 0xFF) * 4;
            return;
        }
        if ((op & 0xFF00) == 0xB000) {  // ADD SP, #+/-imm
            const u32 imm = (op & 0x7F) * 4;
            r[13] = (op & 0x80) ? r[13] - imm : r[13] + imm;
            return;
        }
        if ((op & 0x0600) == 0x0400) return thumbPushPop(op);
        return;  // undefined
    case 6:
        if (!(op & 0x1000)) return thumbMultipleTransfer(op);
        if (((op >> 8) & 15) == 15) return gba_.bios.swi(op & 0xFF);
        if (((op >> 8) & 15) == 14) return;  // undefined
        if (conditionPassed((op >> 8) & 15)) {  // conditional branch
            r[15] += static_cast<s32>(static_cast<s8>(op & 0xFF)) * 2;
            flushed_ = true;
        }
        return;
    case 7:
        switch ((op >> 11) & 3) {
        case 0:  // B: unconditional, 11-bit signed halfword offset
            r[15] += static_cast<s32>(static_cast<u32>(op) << 21) >> 20;
            flushed_ = true;
            return;
        case 2:  // BL part 1: high bits of the offset into LR
            r[14] = r[15] + (static_cast<s32>(static_cast<u32>(op) << 21) >> 9);
            return;
        case 3: {  // BL part 2: jump and set LR to the return address
            const u32 next = (r[15] - 2) | 1;
            r[15] = r[14] + ((op & 0x7FF) << 1);
            r[14] = next;
            flushed_ = true;
            return;
        }
        default: return;  // BLX does not exist on ARMv4
        }
    }
}

void CPU::thumbAlu(u16 op) {
    const int rs = (op >> 3) & 7, rd = op & 7;
    const u32 a = r[rd], b = r[rs];
    bool carry = flagC();
    switch ((op >> 6) & 15) {
    case 0x0: r[rd] = a & b; setNZ(r[rd]); break;                                  // AND
    case 0x1: r[rd] = a ^ b; setNZ(r[rd]); break;                                  // EOR
    case 0x2: r[rd] = shiftByRegister(0, a, b & 0xFF, carry); setNZ(r[rd]); setFlag(kFlagC, carry); idle(1); break;  // LSL
    case 0x3: r[rd] = shiftByRegister(1, a, b & 0xFF, carry); setNZ(r[rd]); setFlag(kFlagC, carry); idle(1); break;  // LSR
    case 0x4: r[rd] = shiftByRegister(2, a, b & 0xFF, carry); setNZ(r[rd]); setFlag(kFlagC, carry); idle(1); break;  // ASR
    case 0x5: r[rd] = add(a, b, flagC() ? 1 : 0, true); break;                     // ADC
    case 0x6: r[rd] = sub(a, b, flagC() ? 1 : 0, true); break;                     // SBC
    case 0x7: r[rd] = shiftByRegister(3, a, b & 0xFF, carry); setNZ(r[rd]); setFlag(kFlagC, carry); idle(1); break;  // ROR
    case 0x8: setNZ(a & b); break;                                                 // TST
    case 0x9: r[rd] = sub(0, b, 1, true); break;                                   // NEG
    case 0xA: sub(a, b, 1, true); break;                                           // CMP
    case 0xB: add(a, b, 0, true); break;                                           // CMN
    case 0xC: r[rd] = a | b; setNZ(r[rd]); break;                                  // ORR
    case 0xD: multiplyCycles(a); r[rd] = a * b; setNZ(r[rd]); break;               // MUL
    case 0xE: r[rd] = a & ~b; setNZ(r[rd]); break;                                 // BIC
    case 0xF: r[rd] = ~b; setNZ(r[rd]); break;                                     // MVN
    }
}

void CPU::thumbHiRegister(u16 op) {
    // These can reach r8-r15: H1/H2 supply the 4th register bit.
    const int rs = ((op >> 3) & 7) | ((op & 0x40) ? 8 : 0);
    const int rd = (op & 7) | ((op & 0x80) ? 8 : 0);
    switch ((op >> 8) & 3) {
    case 0: writeReg(rd, r[rd] + r[rs]); break;  // ADD (no flags)
    case 1: sub(r[rd], r[rs], 1, true); break;   // CMP
    case 2: writeReg(rd, r[rs]); break;          // MOV (no flags)
    case 3: {                                    // BX: switch state on bit 0
        const u32 target = r[rs];
        if (target & 1) { r[15] = target & ~1u; }
        else { cpsr &= ~kFlagT; r[15] = target & ~3u; }
        flushed_ = true;
        break;
    }
    }
}

void CPU::thumbLoadStore(u16 op) {
    Bus& bus = gba_.bus;
    const int rb = (op >> 3) & 7, rd = op & 7;

    if ((op >> 12) == 0x5) {  // register offset
        const u32 addr = r[rb] + r[(op >> 6) & 7];
        switch ((op >> 9) & 7) {
        case 0: bus.write32(addr, r[rd]); break;                                         // STR
        case 1: bus.write16(addr, static_cast<u16>(r[rd])); break;                       // STRH
        case 2: bus.write8(addr, static_cast<u8>(r[rd])); break;                         // STRB
        case 3: r[rd] = static_cast<u32>(static_cast<s8>(bus.read8(addr))); idle(1); break;  // LDSB
        case 4: r[rd] = load32Rotated(addr); idle(1); break;                             // LDR
        case 5: r[rd] = load16Rotated(addr); idle(1); break;                             // LDRH
        case 6: r[rd] = bus.read8(addr); idle(1); break;                                 // LDRB
        case 7: r[rd] = loadSigned16(addr); idle(1); break;                              // LDSH
        }
        return;
    }

    const u32 imm5 = (op >> 6) & 31;
    const bool load = op & 0x800;
    switch (op >> 12) {
    case 0x6: {  // word, immediate offset
        const u32 addr = r[rb] + imm5 * 4;
        if (load) { r[rd] = load32Rotated(addr); idle(1); } else bus.write32(addr, r[rd]);
        return;
    }
    case 0x7: {  // byte, immediate offset
        const u32 addr = r[rb] + imm5;
        if (load) { r[rd] = bus.read8(addr); idle(1); } else bus.write8(addr, static_cast<u8>(r[rd]));
        return;
    }
    case 0x8: {  // halfword, immediate offset
        const u32 addr = r[rb] + imm5 * 2;
        if (load) { r[rd] = load16Rotated(addr); idle(1); } else bus.write16(addr, static_cast<u16>(r[rd]));
        return;
    }
    case 0x9: {  // SP-relative
        const int reg = (op >> 8) & 7;
        const u32 addr = r[13] + (op & 0xFF) * 4;
        if (load) { r[reg] = load32Rotated(addr); idle(1); } else bus.write32(addr, r[reg]);
        return;
    }
    default: return;
    }
}

void CPU::thumbPushPop(u16 op) {
    Bus& bus = gba_.bus;
    const bool pop = op & 0x800;
    const bool extra = op & 0x100;  // PUSH LR / POP PC
    const u32 list = op & 0xFF;

    if (pop) {
        u32 addr = r[13];
        for (int i = 0; i < 8; ++i) {
            if (list & (1u << i)) { r[i] = bus.read32(addr); addr += 4; }
        }
        if (extra) {
            r[15] = bus.read32(addr) & ~1u;
            addr += 4;
            flushed_ = true;
        }
        r[13] = addr;
        idle(1);
    } else {
        const int count = std::popcount(list) + (extra ? 1 : 0);
        u32 addr = r[13] - count * 4;
        r[13] = addr;
        for (int i = 0; i < 8; ++i) {
            if (list & (1u << i)) { bus.write32(addr, r[i]); addr += 4; }
        }
        if (extra) bus.write32(addr, r[14]);
    }
}

void CPU::thumbMultipleTransfer(u16 op) {
    Bus& bus = gba_.bus;
    const bool load = op & 0x800;
    const int rb = (op >> 8) & 7;
    const u32 list = op & 0xFF;
    u32 addr = r[rb];

    if (list == 0) {  // ARM7 quirk: empty list transfers PC, base += 0x40
        if (load) { r[15] = bus.read32(addr); flushed_ = true; }
        else bus.write32(addr, r[15] + 2);
        r[rb] = addr + 0x40;
        return;
    }

    const u32 finalAddr = addr + std::popcount(list) * 4;
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        if (!(list & (1u << i))) continue;
        if (load) r[i] = bus.read32(addr);
        else bus.write32(addr, (i == rb && !first) ? finalAddr : r[i]);
        addr += 4;
        first = false;
    }
    if (load) idle(1);
    // LDMIA does not write back when the base register was loaded.
    if (!(load && (list & (1u << rb)))) r[rb] = finalAddr;
}
