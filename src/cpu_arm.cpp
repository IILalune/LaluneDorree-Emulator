// cpu_arm.cpp - The 32-bit ARM instruction set.
//
// Every ARM instruction is 32 bits. The top 4 bits are a condition code
// (checked before we get here); the remaining bits are matched against
// patterns to find out which kind of instruction it is.
#include <bit>

#include "cpu.hpp"
#include "gba.hpp"

void CPU::executeArm(u32 op) {
    // The order of these checks matters: several encodings overlap and the
    // more specific patterns must be tested first.
    if ((op & 0x0FFFFFF0) == 0x012FFF10) return armBranchExchange(op);
    if ((op & 0x0FC000F0) == 0x00000090) return armMultiply(op);
    if ((op & 0x0F8000F0) == 0x00800090) return armMultiplyLong(op);
    if ((op & 0x0FB00FF0) == 0x01000090) return armSwap(op);
    if ((op & 0x0E000090) == 0x00000090 && (op & 0x60)) return armHalfwordTransfer(op);
    if ((op & 0x0FBF0FFF) == 0x010F0000 || (op & 0x0DB0F000) == 0x0120F000) return armPsrTransfer(op);
    if ((op & 0x0C000000) == 0x00000000) return armDataProcessing(op);
    if ((op & 0x0E000010) == 0x06000010) return;  // undefined instruction: ignore
    if ((op & 0x0C000000) == 0x04000000) return armSingleTransfer(op);
    if ((op & 0x0E000000) == 0x08000000) return armBlockTransfer(op);
    if ((op & 0x0E000000) == 0x0A000000) return armBranch(op);
    if ((op & 0x0F000000) == 0x0F000000) return gba_.bios.swi((op >> 16) & 0xFF);
    // Coprocessor instructions: the GBA has no coprocessors, so ignore them.
}

void CPU::armBranchExchange(u32 op) {
    const u32 target = r[op & 15];
    if (target & 1) { cpsr |= kFlagT; r[15] = target & ~1u; }
    else { cpsr &= ~kFlagT; r[15] = target & ~3u; }
    flushed_ = true;
}

void CPU::armBranch(u32 op) {
    // 24-bit signed word offset. Shifting left by 8 then arithmetic-right by 6
    // sign-extends it and multiplies by 4 in one go.
    const s32 offset = static_cast<s32>(op << 8) >> 6;
    if (op & (1 << 24)) r[14] = r[15] - 4;  // BL: link = next instruction
    r[15] += offset;
    flushed_ = true;
}

void CPU::armDataProcessing(u32 op) {
    const bool immediate = op & (1 << 25);
    const u32 opcode = (op >> 21) & 15;
    const bool setFlags = op & (1 << 20);
    const int rn = (op >> 16) & 15;
    const int rd = (op >> 12) & 15;

    u32 operand1 = r[rn];
    u32 operand2;
    bool shifterCarry = flagC();

    if (immediate) {
        const u32 rotate = ((op >> 8) & 15) * 2;
        operand2 = ror32(op & 0xFF, rotate);
        if (rotate) shifterCarry = operand2 >> 31;
    } else {
        const int rm = op & 15;
        const u32 type = (op >> 5) & 3;
        u32 value = r[rm];
        if (op & 0x10) {
            // Shift by register takes an extra cycle, during which the PC
            // advances one more step (so PC reads as +12).
            const u32 amount = r[(op >> 8) & 15] & 0xFF;
            if (rm == 15) value += 4;
            if (rn == 15) operand1 += 4;
            idle(1);
            operand2 = shiftByRegister(type, value, amount, shifterCarry);
        } else {
            operand2 = shiftByImmediate(type, value, (op >> 7) & 31, shifterCarry);
        }
    }

    // When the destination is r15 with S set, CPSR is restored from SPSR
    // (this is how exception handlers return), so normal flags are skipped.
    const bool restoreCpsr = setFlags && rd == 15;
    const bool flags = setFlags && !restoreCpsr;
    const u32 carry = flagC() ? 1 : 0;

    auto logical = [&](u32 result) {
        if (flags) { setNZ(result); setFlag(kFlagC, shifterCarry); }
        return result;
    };

    u32 result = 0;
    bool writeResult = true;
    switch (opcode) {
    case 0x0: result = logical(operand1 & operand2); break;            // AND
    case 0x1: result = logical(operand1 ^ operand2); break;            // EOR
    case 0x2: result = sub(operand1, operand2, 1, flags); break;       // SUB
    case 0x3: result = sub(operand2, operand1, 1, flags); break;       // RSB
    case 0x4: result = add(operand1, operand2, 0, flags); break;       // ADD
    case 0x5: result = add(operand1, operand2, carry, flags); break;   // ADC
    case 0x6: result = sub(operand1, operand2, carry, flags); break;   // SBC
    case 0x7: result = sub(operand2, operand1, carry, flags); break;   // RSC
    case 0x8: logical(operand1 & operand2); writeResult = false; break; // TST
    case 0x9: logical(operand1 ^ operand2); writeResult = false; break; // TEQ
    case 0xA: sub(operand1, operand2, 1, flags); writeResult = false; break; // CMP
    case 0xB: add(operand1, operand2, 0, flags); writeResult = false; break; // CMN
    case 0xC: result = logical(operand1 | operand2); break;            // ORR
    case 0xD: result = logical(operand2); break;                       // MOV
    case 0xE: result = logical(operand1 & ~operand2); break;           // BIC
    case 0xF: result = logical(~operand2); break;                      // MVN
    }

    if (writeResult) writeReg(rd, result);
    if (restoreCpsr) setCpsr(spsr());
}

void CPU::armPsrTransfer(u32 op) {
    const bool useSpsr = op & (1 << 22);
    if (!(op & (1 << 21))) {  // MRS: read status register
        r[(op >> 12) & 15] = useSpsr ? spsr() : cpsr;
        return;
    }
    // MSR: write status register, restricted to the selected byte fields.
    u32 value;
    if (op & (1 << 25)) value = ror32(op & 0xFF, ((op >> 8) & 15) * 2);
    else value = r[op & 15];

    u32 mask = 0;
    if (op & (1 << 19)) mask |= 0xFF000000;  // flags
    if (op & (1 << 18)) mask |= 0x00FF0000;
    if (op & (1 << 17)) mask |= 0x0000FF00;
    if (op & (1 << 16)) mask |= 0x000000FF;  // control (mode, I, T)

    if (useSpsr) {
        setSpsr((spsr() & ~mask) | (value & mask));
    } else {
        if ((cpsr & 0x1F) == kModeUser) mask &= 0xFF000000;  // user mode can only change flags
        setCpsr((cpsr & ~mask) | (value & mask));
    }
}

void CPU::armMultiply(u32 op) {
    const int rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    u32 result = r[rm] * r[rs];
    multiplyCycles(r[rs]);
    if (op & (1 << 21)) { result += r[rn]; idle(1); }  // MLA: accumulate
    r[rd] = result;
    if (op & (1 << 20)) setNZ(result);
}

void CPU::armMultiplyLong(u32 op) {
    const int rdHi = (op >> 16) & 15, rdLo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
    const bool isSigned = op & (1 << 22);
    u64 result;
    if (isSigned) {
        result = static_cast<u64>(static_cast<s64>(static_cast<s32>(r[rm])) *
                                  static_cast<s64>(static_cast<s32>(r[rs])));
    } else {
        result = static_cast<u64>(r[rm]) * r[rs];
    }
    multiplyCycles(r[rs]);
    idle(1);
    if (op & (1 << 21)) {  // accumulate
        result += (static_cast<u64>(r[rdHi]) << 32) | r[rdLo];
        idle(1);
    }
    r[rdLo] = static_cast<u32>(result);
    r[rdHi] = static_cast<u32>(result >> 32);
    if (op & (1 << 20)) {
        setFlag(kFlagN, result >> 63);
        setFlag(kFlagZ, result == 0);
    }
}

void CPU::armSwap(u32 op) {
    const int rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
    const u32 addr = r[rn];
    const u32 source = r[rm];
    if (op & (1 << 22)) {
        const u32 old = gba_.bus.read8(addr);
        gba_.bus.write8(addr, static_cast<u8>(source));
        writeReg(rd, old);
    } else {
        const u32 old = load32Rotated(addr);
        gba_.bus.write32(addr, source);
        writeReg(rd, old);
    }
    idle(1);
}

void CPU::armHalfwordTransfer(u32 op) {
    const bool pre = op & (1 << 24), up = op & (1 << 23), immediate = op & (1 << 22);
    const bool writeback = op & (1 << 21), load = op & (1 << 20);
    const int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    const u32 kind = (op >> 5) & 3;

    const u32 offset = immediate ? (((op >> 4) & 0xF0) | (op & 0xF)) : r[op & 15];
    const u32 base = r[rn];
    const u32 offsetAddr = up ? base + offset : base - offset;
    const u32 addr = pre ? offsetAddr : base;

    if (load) {
        u32 value = 0;
        switch (kind) {
        case 1: value = load16Rotated(addr); break;                                       // LDRH
        case 2: value = static_cast<u32>(static_cast<s8>(gba_.bus.read8(addr))); break;   // LDRSB
        case 3: value = loadSigned16(addr); break;                                        // LDRSH
        }
        idle(1);
        if (!pre || writeback) writeReg(rn, offsetAddr);
        writeReg(rd, value);  // a loaded value wins over writeback when rn == rd
    } else {
        if (kind == 1) {  // STRH
            const u32 value = r[rd] + (rd == 15 ? 4 : 0);
            gba_.bus.write16(addr, static_cast<u16>(value));
        }
        if (!pre || writeback) writeReg(rn, offsetAddr);
    }
}

void CPU::armSingleTransfer(u32 op) {
    const bool registerOffset = op & (1 << 25);
    const bool pre = op & (1 << 24), up = op & (1 << 23), byte = op & (1 << 22);
    const bool writeback = op & (1 << 21), load = op & (1 << 20);
    const int rn = (op >> 16) & 15, rd = (op >> 12) & 15;

    u32 offset;
    if (registerOffset) {
        bool carry = flagC();
        offset = shiftByImmediate((op >> 5) & 3, r[op & 15], (op >> 7) & 31, carry);
    } else {
        offset = op & 0xFFF;
    }
    const u32 base = r[rn];
    const u32 offsetAddr = up ? base + offset : base - offset;
    const u32 addr = pre ? offsetAddr : base;

    if (load) {
        const u32 value = byte ? gba_.bus.read8(addr) : load32Rotated(addr);
        idle(1);
        if (!pre || writeback) writeReg(rn, offsetAddr);
        writeReg(rd, value);
    } else {
        const u32 value = r[rd] + (rd == 15 ? 4 : 0);  // STR PC stores PC+12
        if (byte) gba_.bus.write8(addr, static_cast<u8>(value));
        else gba_.bus.write32(addr, value);
        if (!pre || writeback) writeReg(rn, offsetAddr);
    }
}

void CPU::armBlockTransfer(u32 op) {
    const bool pre = op & (1 << 24), up = op & (1 << 23), psrOrUser = op & (1 << 22);
    const bool writeback = op & (1 << 21), load = op & (1 << 20);
    const int rn = (op >> 16) & 15;
    u32 list = op & 0xFFFF;

    // ARM7 quirk: an empty list transfers r15 but moves the base by 64 bytes.
    int count = std::popcount(list);
    const bool emptyList = count == 0;
    if (emptyList) { list = 0x8000; count = 16; }

    const u32 base = r[rn];
    u32 addr, finalAddr;
    if (up) { addr = base + (pre ? 4 : 0); finalAddr = base + count * 4; }
    else { addr = base - count * 4 + (pre ? 0 : 4); finalAddr = base - count * 4; }

    // LDM with r15 and S set = return from exception. Otherwise S means
    // "transfer the user-mode registers".
    const bool userBank = psrOrUser && !(load && (list & 0x8000));
    const u32 oldMode = cpsr & 0x1F;
    if (userBank) switchMode(kModeUser);

    bool first = true;
    for (int i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        if (load) {
            const u32 value = gba_.bus.read32(addr);
            if (i == 15) { r[15] = value; flushed_ = true; }
            else r[i] = value;
        } else {
            u32 value = r[i];
            if (i == 15) value += 4;
            // Storing the base register: the old value if it is first in the
            // list, otherwise the already-updated value.
            if (i == rn && !first && writeback) value = finalAddr;
            gba_.bus.write32(addr, value);
        }
        first = false;
        addr += 4;
    }
    if (load) idle(1);
    if (userBank) switchMode(oldMode);

    if (writeback && !(load && (list & (1u << rn)) && !emptyList)) r[rn] = finalAddr;
    if (load && psrOrUser && (list & 0x8000)) setCpsr(spsr());
}
