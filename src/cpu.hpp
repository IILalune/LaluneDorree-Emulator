// cpu.hpp - ARM7TDMI interpreter.
//
// The CPU has 16 visible registers (r0-r15; r13 = stack pointer, r14 = link
// register, r15 = program counter) plus a status register, CPSR, holding the
// condition flags (N, Z, C, V), the interrupt-disable bit and the current
// mode. Some registers are "banked": each privileged mode (IRQ, supervisor...)
// has its own private copy of r13/r14 so an interrupt handler gets its own
// stack.
//
// Pipeline model: real hardware fetches two instructions ahead, so reading r15
// gives "current instruction + 8" in ARM state (+4 in Thumb). We mimic that by
// keeping r15 two instructions ahead of the instruction being executed.
#pragma once

#include "common.hpp"

class GBA;
class StateStream;

class CPU {
public:
    explicit CPU(GBA& gba);

    void reset();   // puts the CPU in the state the real BIOS leaves it in
    void step();    // execute one instruction (or take a pending interrupt)
    void serialize(StateStream& s);  // see state.hpp

    // Branch to `addr` in the current state (used by the HLE BIOS).
    void jumpTo(u32 addr);
    // Address of the instruction currently executing.
    u32 currentInstruction() const { return r[15] - (thumb() ? 4 : 8); }

    bool thumb() const { return cpsr & kFlagT; }
    bool halted = false;

    u32 r[16]{};
    u32 cpsr = 0;
    u32 openBus = 0;  // last fetched opcode (what unmapped reads return)

    static constexpr u32 kFlagN = 1u << 31;
    static constexpr u32 kFlagZ = 1u << 30;
    static constexpr u32 kFlagC = 1u << 29;
    static constexpr u32 kFlagV = 1u << 28;
    static constexpr u32 kFlagI = 1u << 7;
    static constexpr u32 kFlagT = 1u << 5;

    static constexpr u32 kModeUser = 0x10, kModeFiq = 0x11, kModeIrq = 0x12, kModeSvc = 0x13,
                         kModeAbort = 0x17, kModeUndef = 0x1B, kModeSystem = 0x1F;

    void setSpsrFor(u32 mode, u32 value);
    void setBankedSp(u32 mode, u32 value);

private:
    GBA& gba_;
    bool flushed_ = false;    // set when an instruction writes r15
    bool nextSequential_ = false;

    // Banked registers: index 0 = user/system, 1 = FIQ, 2 = IRQ, 3 = SVC,
    // 4 = abort, 5 = undefined.
    u32 bankR13_[6]{}, bankR14_[6]{}, bankSpsr_[6]{};
    u32 userR8to12_[5]{}, fiqR8to12_[5]{};

    static int bankIndex(u32 mode);
    void switchMode(u32 newMode);
    void setCpsr(u32 value);
    u32 spsr() const;
    void setSpsr(u32 value);
    void enterIrq();

    // Flags
    bool flagC() const { return cpsr & kFlagC; }
    void setNZ(u32 result);
    void setFlag(u32 flag, bool on) { cpsr = on ? (cpsr | flag) : (cpsr & ~flag); }
    bool conditionPassed(u32 cond) const;

    // Arithmetic helpers shared by ARM and Thumb.
    u32 add(u32 a, u32 b, u32 carryIn, bool setFlags);
    u32 sub(u32 a, u32 b, u32 carryIn, bool setFlags);  // a - b - !carryIn
    static u32 shiftByImmediate(u32 type, u32 value, u32 amount, bool& carry);
    static u32 shiftByRegister(u32 type, u32 value, u32 amount, bool& carry);
    void writeReg(int n, u32 value) { r[n] = value; if (n == 15) flushed_ = true; }
    void idle(int cycles);
    void multiplyCycles(u32 operand);

    // Memory helpers (go through the bus, which counts cycles).
    u32 load32Rotated(u32 addr);
    u32 load16Rotated(u32 addr);
    u32 loadSigned16(u32 addr);

    // ARM instruction groups (cpu_arm.cpp)
    void executeArm(u32 op);
    void armBranchExchange(u32 op);
    void armBranch(u32 op);
    void armDataProcessing(u32 op);
    void armPsrTransfer(u32 op);
    void armMultiply(u32 op);
    void armMultiplyLong(u32 op);
    void armSwap(u32 op);
    void armHalfwordTransfer(u32 op);
    void armSingleTransfer(u32 op);
    void armBlockTransfer(u32 op);

    // Thumb instruction groups (cpu_thumb.cpp)
    void executeThumb(u16 op);
    void thumbAlu(u16 op);
    void thumbHiRegister(u16 op);
    void thumbLoadStore(u16 op);
    void thumbPushPop(u16 op);
    void thumbMultipleTransfer(u16 op);
};
