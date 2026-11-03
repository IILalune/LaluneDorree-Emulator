// bios.hpp - High-level emulation (HLE) of the GBA BIOS.
//
// Games call into the BIOS with the SWI ("software interrupt") instruction to
// do things like division, decompression, or waiting for the next frame.
// Instead of running Nintendo's copyrighted BIOS code, we implement each of
// those functions directly in C++.
#pragma once

#include "common.hpp"

class GBA;

class Bios {
public:
    explicit Bios(GBA& gba);

    // Writes a tiny replacement BIOS image (interrupt dispatcher) into memory.
    void install();
    void swi(u32 number);

private:
    GBA& gba_;
    bool waitingForInterrupt_ = false;

    void softReset();
    void registerRamReset(u32 flags);
    void intrWait(bool discardOld, u16 mask);
    void div(s32 numerator, s32 denominator);
    void sqrt();
    void arcTan();
    void arcTan2();
    void cpuSet();
    void cpuFastSet();
    void bgAffineSet();
    void objAffineSet();
    void bitUnPack();
    void lz77(bool vram);
    void huffman();
    void runLength(bool vram);
    void diffUnFilter(int unitBytes, bool vram);
    void midiKeyToFreq();

    void writeOutput(u32& dst, const u8* data, std::size_t size, bool vram);
};
