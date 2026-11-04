#include "cpu.hpp"

#include "gba.hpp"
#include "state.hpp"

CPU::CPU(GBA& gba) : gba_(gba) {}

void CPU::reset() {
    for (auto& reg : r) reg = 0;
    for (int i = 0; i < 6; ++i) bankR13_[i] = bankR14_[i] = bankSpsr_[i] = 0;
    for (int i = 0; i < 5; ++i) userR8to12_[i] = fiqR8to12_[i] = 0;

    // Stack pointers the real BIOS sets up before jumping to the cartridge.
    cpsr = kModeSystem;
    bankR13_[bankIndex(kModeSvc)] = 0x03007FE0;
    bankR13_[bankIndex(kModeIrq)] = 0x03007FA0;
    r[13] = 0x03007F00;

    r[15] = 0x08000000 + 8;  // start of the cartridge, ARM state
    halted = false;
    flushed_ = false;
    nextSequential_ = false;
}

int CPU::bankIndex(u32 mode) {
    switch (mode) {
    case kModeFiq: return 1;
    case kModeIrq: return 2;
    case kModeSvc: return 3;
    case kModeAbort: return 4;
    case kModeUndef: return 5;
    default: return 0;  // user and system share registers
    }
}

void CPU::switchMode(u32 newMode) {
    const u32 oldMode = cpsr & 0x1F;
    const int oldBank = bankIndex(oldMode);
    const int newBank = bankIndex(newMode);
    if (oldBank != newBank) {
        bankR13_[oldBank] = r[13];
        bankR14_[oldBank] = r[14];
        // FIQ mode also banks r8-r12.
        if (oldBank == 1) {
            for (int i = 0; i < 5; ++i) { fiqR8to12_[i] = r[8 + i]; r[8 + i] = userR8to12_[i]; }
        } else if (newBank == 1) {
            for (int i = 0; i < 5; ++i) { userR8to12_[i] = r[8 + i]; r[8 + i] = fiqR8to12_[i]; }
        }
        r[13] = bankR13_[newBank];
        r[14] = bankR14_[newBank];
    }
    cpsr = (cpsr & ~0x1Fu) | newMode;
}

void CPU::setCpsr(u32 value) {
    switchMode(value & 0x1F);
    cpsr = value;
}

u32 CPU::spsr() const {
    const int bank = bankIndex(cpsr & 0x1F);
    return bank == 0 ? cpsr : bankSpsr_[bank];  // user mode has no SPSR
}

void CPU::setSpsr(u32 value) {
    const int bank = bankIndex(cpsr & 0x1F);
    if (bank != 0) bankSpsr_[bank] = value;
}

void CPU::setSpsrFor(u32 mode, u32 value) { bankSpsr_[bankIndex(mode)] = value; }

void CPU::setBankedSp(u32 mode, u32 value) {
    if (bankIndex(mode) == bankIndex(cpsr & 0x1F)) r[13] = value;
    else bankR13_[bankIndex(mode)] = value;
}

void CPU::jumpTo(u32 addr) {
    r[15] = addr;
    flushed_ = true;
}

void CPU::enterIrq() {
    const u32 nextInstruction = r[15] - (thumb() ? 4 : 8);
    const u32 oldCpsr = cpsr;
    switchMode(kModeIrq);
    setSpsr(oldCpsr);
    r[14] = nextInstruction + 4;     // the handler returns with SUBS PC, LR, #4
    cpsr = (cpsr & ~kFlagT) | kFlagI;
    r[15] = 0x18 + 8;                // IRQ vector in the BIOS (ARM state)
    nextSequential_ = false;
}

void CPU::step() {
    Bus& bus = gba_.bus;

    // Interrupt check: master enable (IME), enabled & requested, CPSR.I clear.
    if ((bus.reg(0x208) & 1) && (bus.reg(0x200) & bus.reg(0x202) & 0x3FFF) && !(cpsr & kFlagI)) {
        enterIrq();
    }

    flushed_ = false;
    if (thumb()) {
        const u32 pc = r[15] - 4;
        const u16 op = bus.fetch16(pc, nextSequential_);
        openBus = op | (static_cast<u32>(op) << 16);
        executeThumb(op);
    } else {
        const u32 pc = r[15] - 8;
        const u32 op = bus.fetch32(pc, nextSequential_);
        openBus = op;
        if (conditionPassed(op >> 28)) executeArm(op);
    }

    if (flushed_) {
        // The pipeline refills from the new address. The T bit may have
        // changed (BX), so align for whichever state we are in now.
        if (thumb()) r[15] = (r[15] & ~1u) + 4;
        else r[15] = (r[15] & ~3u) + 8;
        nextSequential_ = false;
    } else {
        r[15] += thumb() ? 2 : 4;
        nextSequential_ = true;
    }
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void CPU::idle(int cycles) { gba_.bus.cycles += cycles; }

void CPU::multiplyCycles(u32 operand) {
    // The multiplier finishes early when the top bytes are all 0s or all 1s.
    int m = 4;
    if ((operand & 0xFFFFFF00) == 0 || (operand & 0xFFFFFF00) == 0xFFFFFF00) m = 1;
    else if ((operand & 0xFFFF0000) == 0 || (operand & 0xFFFF0000) == 0xFFFF0000) m = 2;
    else if ((operand & 0xFF000000) == 0 || (operand & 0xFF000000) == 0xFF000000) m = 3;
    idle(m);
}

void CPU::setNZ(u32 result) {
    cpsr &= ~(kFlagN | kFlagZ);
    cpsr |= result & kFlagN;
    if (result == 0) cpsr |= kFlagZ;
}

bool CPU::conditionPassed(u32 cond) const {
    const bool n = cpsr & kFlagN, z = cpsr & kFlagZ, c = cpsr & kFlagC, v = cpsr & kFlagV;
    switch (cond) {
    case 0x0: return z;            // EQ
    case 0x1: return !z;           // NE
    case 0x2: return c;            // CS
    case 0x3: return !c;           // CC
    case 0x4: return n;            // MI
    case 0x5: return !n;           // PL
    case 0x6: return v;            // VS
    case 0x7: return !v;           // VC
    case 0x8: return c && !z;      // HI
    case 0x9: return !c || z;      // LS
    case 0xA: return n == v;       // GE
    case 0xB: return n != v;       // LT
    case 0xC: return !z && n == v; // GT
    case 0xD: return z || n != v;  // LE
    case 0xE: return true;         // AL
    default: return false;         // NV
    }
}

u32 CPU::add(u32 a, u32 b, u32 carryIn, bool setFlags) {
    const u64 wide = static_cast<u64>(a) + b + carryIn;
    const u32 result = static_cast<u32>(wide);
    if (setFlags) {
        setNZ(result);
        setFlag(kFlagC, wide >> 32);
        setFlag(kFlagV, (~(a ^ b) & (a ^ result)) >> 31);  // same-sign inputs, different-sign output
    }
    return result;
}

u32 CPU::sub(u32 a, u32 b, u32 carryIn, bool setFlags) {
    const u32 borrow = 1 - carryIn;
    const u32 result = a - b - borrow;
    if (setFlags) {
        setNZ(result);
        setFlag(kFlagC, static_cast<u64>(a) >= static_cast<u64>(b) + borrow);  // C = "no borrow"
        setFlag(kFlagV, ((a ^ b) & (a ^ result)) >> 31);
    }
    return result;
}

u32 CPU::shiftByImmediate(u32 type, u32 value, u32 amount, bool& carry) {
    switch (type) {
    case 0:  // LSL
        if (amount == 0) return value;
        carry = (value >> (32 - amount)) & 1;
        return value << amount;
    case 1:  // LSR (#0 encodes #32)
        if (amount == 0) { carry = value >> 31; return 0; }
        carry = (value >> (amount - 1)) & 1;
        return value >> amount;
    case 2:  // ASR (#0 encodes #32)
        if (amount == 0) { carry = value >> 31; return static_cast<u32>(static_cast<s32>(value) >> 31); }
        carry = (value >> (amount - 1)) & 1;
        return static_cast<u32>(static_cast<s32>(value) >> amount);
    default:  // ROR (#0 encodes RRX: rotate right through carry)
        if (amount == 0) {
            const bool newCarry = value & 1;
            const u32 result = (static_cast<u32>(carry) << 31) | (value >> 1);
            carry = newCarry;
            return result;
        }
        carry = (value >> (amount - 1)) & 1;
        return ror32(value, amount);
    }
}

u32 CPU::shiftByRegister(u32 type, u32 value, u32 amount, bool& carry) {
    if (amount == 0) return value;  // carry unchanged
    switch (type) {
    case 0:  // LSL
        if (amount < 32) { carry = (value >> (32 - amount)) & 1; return value << amount; }
        carry = amount == 32 ? (value & 1) : 0;
        return 0;
    case 1:  // LSR
        if (amount < 32) { carry = (value >> (amount - 1)) & 1; return value >> amount; }
        carry = amount == 32 ? (value >> 31) : 0;
        return 0;
    case 2:  // ASR
        if (amount < 32) {
            carry = (value >> (amount - 1)) & 1;
            return static_cast<u32>(static_cast<s32>(value) >> amount);
        }
        carry = value >> 31;
        return static_cast<u32>(static_cast<s32>(value) >> 31);
    default:  // ROR
        amount &= 31;
        if (amount == 0) { carry = value >> 31; return value; }
        carry = (value >> (amount - 1)) & 1;
        return ror32(value, amount);
    }
}

// Misaligned word loads rotate the data on the ARM7.
u32 CPU::load32Rotated(u32 addr) { return ror32(gba_.bus.read32(addr), (addr & 3) * 8); }
u32 CPU::load16Rotated(u32 addr) { return ror32(gba_.bus.read16(addr), (addr & 1) * 8); }
u32 CPU::loadSigned16(u32 addr) {
    // A misaligned signed halfword load behaves like a signed byte load.
    if (addr & 1) return static_cast<u32>(static_cast<s8>(gba_.bus.read8(addr)));
    return static_cast<u32>(static_cast<s16>(gba_.bus.read16(addr)));
}

void CPU::serialize(StateStream& s) {
    s.section("CPU ");
    s.value(r);
    s.value(cpsr);
    s.value(openBus);
    s.value(halted);
    s.value(flushed_);
    s.value(nextSequential_);
    s.value(bankR13_);
    s.value(bankR14_);
    s.value(bankSpsr_);
    s.value(userR8to12_);
    s.value(fiqR8to12_);
}
