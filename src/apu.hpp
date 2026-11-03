// apu.hpp - Audio Processing Unit.
//
// The GBA has the four classic Game Boy channels (two square waves, a
// programmable wave channel, and noise) plus two "Direct Sound" channels that
// play 8-bit samples streamed into FIFOs by DMA. We generate stereo 16-bit
// samples at 48 kHz for the host's audio device.
#pragma once

#include <array>
#include <vector>

#include "common.hpp"

class GBA;

class APU {
public:
    static constexpr int kSampleRate = 48000;

    explicit APU(GBA& gba);
    void reset();
    void tick(int cycles);

    void writeRegister(u32 offset, u16 value);
    void writeFifo(int fifo, u8 value);
    u16 readWaveRam(u32 offset) const;
    void writeWaveRam(u32 offset, u16 value);
    void onTimerOverflow(int timer);
    u16 channelStatus() const;

    // Interleaved stereo samples (L, R, L, R, ...) produced since last drained.
    std::vector<s16> samples;

private:
    struct Envelope {
        int initialVolume = 0, volume = 0, period = 0, timer = 0;
        bool increase = false;
        void trigger() { volume = initialVolume; timer = period; }
        void clock();
        bool dacEnabled() const { return initialVolume != 0 || increase; }
    };

    struct Square {
        bool on = false;
        int duty = 0, step = 0, frequency = 0, timer = 0;
        int length = 0;
        bool lengthEnabled = false;
        Envelope envelope;
        // Sweep (channel 1 only)
        int sweepPeriod = 0, sweepShift = 0, sweepTimer = 0, shadowFrequency = 0;
        bool sweepDecrease = false, sweepEnabled = false;
    };

    struct Wave {
        bool on = false, dacEnabled = false, twoBanks = false;
        int bank = 0, position = 0, frequency = 0, timer = 0;
        int length = 0, volumeCode = 0;
        bool lengthEnabled = false, force75 = false;
    };

    struct Noise {
        bool on = false;
        int length = 0, divisor = 0, shift = 0, timer = 0;
        bool lengthEnabled = false, narrow = false;
        u16 lfsr = 0x7FFF;
        Envelope envelope;
    };

    struct Fifo {
        std::array<s8, 32> data{};
        int readPos = 0, writePos = 0, count = 0;
        s8 current = 0;
        void reset() { readPos = writePos = count = 0; }
    };

    GBA& gba_;
    Square square_[2];
    Wave wave_;
    Noise noise_;
    Fifo fifo_[2];
    std::array<std::array<u8, 16>, 2> waveRam_{};

    u64 sampleAccumulator_ = 0;
    int frameSequencerCycles_ = 0;
    int frameSequencerStep_ = 0;
    int pendingCycles_ = 0;

    void clockFrameSequencer();
    void clockSweep();
    int sweepCalculate();
    void advanceChannels(int cycles);
    void mixSample();
    void triggerSquare(int i);
    int squarePeriod(int i) const { return (2048 - square_[i].frequency) * 16; }
    int wavePeriod() const { return (2048 - wave_.frequency) * 8; }
    // Noise clock = 524288 Hz / r / 2^(s+1), where r = 0 counts as 0.5.
    int noisePeriod() const { return (noise_.divisor == 0 ? 16 : noise_.divisor * 32) << (noise_.shift + 1); }
};
