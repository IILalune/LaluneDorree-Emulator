#include "apu.hpp"

#include <algorithm>

#include "gba.hpp"
#include "state.hpp"

namespace {
// Square wave shapes: 12.5%, 25%, 50%, 75% duty cycle.
constexpr u8 kDutyTable[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
};
constexpr int kFrameSequencerPeriod = kCpuFrequency / 512;  // 512 Hz
}  // namespace

APU::APU(GBA& gba) : gba_(gba) {}

void APU::reset() {
    square_[0] = square_[1] = Square{};
    wave_ = Wave{};
    noise_ = Noise{};
    fifo_[0] = fifo_[1] = Fifo{};
    for (auto& bank : waveRam_) bank.fill(0);
    sampleAccumulator_ = 0;
    frameSequencerCycles_ = 0;
    frameSequencerStep_ = 0;
    pendingCycles_ = 0;
    samples.clear();
}

void APU::Envelope::clock() {
    if (period == 0) return;
    if (--timer > 0) return;
    timer = period;
    if (increase && volume < 15) ++volume;
    else if (!increase && volume > 0) --volume;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

void APU::writeRegister(u32 offset, u16 value) {
    auto setEnvelope = [](Envelope& env, u16 v) {
        env.period = (v >> 8) & 7;
        env.increase = v & 0x800;
        env.initialVolume = v >> 12;
    };

    switch (offset) {
    case 0x60:  // SOUND1CNT_L: sweep
        square_[0].sweepShift = value & 7;
        square_[0].sweepDecrease = value & 8;
        square_[0].sweepPeriod = (value >> 4) & 7;
        break;
    case 0x62: case 0x68: {  // SOUNDxCNT_H: duty, length, envelope
        Square& sq = square_[offset == 0x62 ? 0 : 1];
        sq.length = 64 - (value & 63);
        sq.duty = (value >> 6) & 3;
        setEnvelope(sq.envelope, value);
        if (!sq.envelope.dacEnabled()) sq.on = false;
        break;
    }
    case 0x64: case 0x6C: {  // SOUNDxCNT_X: frequency, trigger
        const int i = offset == 0x64 ? 0 : 1;
        square_[i].frequency = value & 0x7FF;
        square_[i].lengthEnabled = value & 0x4000;
        if (value & 0x8000) triggerSquare(i);
        break;
    }
    case 0x70:  // SOUND3CNT_L: wave control
        wave_.twoBanks = value & 0x20;
        wave_.bank = (value >> 6) & 1;
        wave_.dacEnabled = value & 0x80;
        if (!wave_.dacEnabled) wave_.on = false;
        break;
    case 0x72:  // SOUND3CNT_H: length, volume
        wave_.length = 256 - (value & 0xFF);
        wave_.volumeCode = (value >> 13) & 3;
        wave_.force75 = value & 0x8000;
        break;
    case 0x74:  // SOUND3CNT_X: frequency, trigger
        wave_.frequency = value & 0x7FF;
        wave_.lengthEnabled = value & 0x4000;
        if (value & 0x8000) {
            wave_.on = wave_.dacEnabled;
            if (wave_.length == 0) wave_.length = 256;
            wave_.position = 0;
            wave_.timer = wavePeriod();
        }
        break;
    case 0x78:  // SOUND4CNT_L: length, envelope
        noise_.length = 64 - (value & 63);
        setEnvelope(noise_.envelope, value);
        if (!noise_.envelope.dacEnabled()) noise_.on = false;
        break;
    case 0x7C:  // SOUND4CNT_H: frequency, trigger
        noise_.divisor = value & 7;
        noise_.narrow = value & 8;
        noise_.shift = (value >> 4) & 15;
        noise_.lengthEnabled = value & 0x4000;
        if (value & 0x8000) {
            noise_.on = noise_.envelope.dacEnabled();
            if (noise_.length == 0) noise_.length = 64;
            noise_.envelope.trigger();
            noise_.lfsr = 0x7FFF;
            noise_.timer = noisePeriod();
        }
        break;
    case 0x82:  // SOUNDCNT_H: FIFO reset bits are "write 1 to reset"
        if (value & 0x0800) fifo_[0].reset();
        if (value & 0x8000) fifo_[1].reset();
        gba_.bus.reg(0x82) = value & 0x770F;
        break;
    case 0x84:  // SOUNDCNT_X: master enable
        if (!(value & 0x80)) {
            square_[0].on = square_[1].on = wave_.on = noise_.on = false;
        }
        break;
    default: break;
    }
}

void APU::triggerSquare(int i) {
    Square& sq = square_[i];
    sq.on = sq.envelope.dacEnabled();
    if (sq.length == 0) sq.length = 64;
    sq.timer = squarePeriod(i);
    sq.envelope.trigger();
    if (i == 0) {
        sq.shadowFrequency = sq.frequency;
        sq.sweepTimer = sq.sweepPeriod ? sq.sweepPeriod : 8;
        sq.sweepEnabled = sq.sweepPeriod != 0 || sq.sweepShift != 0;
        if (sq.sweepShift) sweepCalculate();
    }
}

u16 APU::readWaveRam(u32 offset) const {
    // The CPU sees the bank that is *not* currently playing.
    const auto& bank = waveRam_[wave_.bank ^ 1];
    return static_cast<u16>(bank[offset & 15] | (bank[(offset + 1) & 15] << 8));
}

void APU::writeWaveRam(u32 offset, u16 value) {
    auto& bank = waveRam_[wave_.bank ^ 1];
    bank[offset & 15] = static_cast<u8>(value);
    bank[(offset + 1) & 15] = static_cast<u8>(value >> 8);
}

void APU::writeFifo(int fifo, u8 value) {
    Fifo& q = fifo_[fifo];
    if (q.count >= 32) return;
    q.data[q.writePos] = static_cast<s8>(value);
    q.writePos = (q.writePos + 1) & 31;
    ++q.count;
}

void APU::onTimerOverflow(int timer) {
    const u16 cntH = gba_.bus.reg(0x82);
    for (int f = 0; f < 2; ++f) {
        const int selected = (cntH >> (10 + f * 4)) & 1;
        if (selected != timer) continue;
        Fifo& q = fifo_[f];
        if (q.count > 0) {
            q.current = q.data[q.readPos];
            q.readPos = (q.readPos + 1) & 31;
            --q.count;
        }
        if (q.count <= 16) gba_.dma.onFifoRequest(f);  // ask DMA for more samples
    }
}

u16 APU::channelStatus() const {
    return static_cast<u16>((square_[0].on ? 1 : 0) | (square_[1].on ? 2 : 0) | (wave_.on ? 4 : 0) |
                            (noise_.on ? 8 : 0));
}

// ---------------------------------------------------------------------------
// Sound generation
// ---------------------------------------------------------------------------

int APU::sweepCalculate() {
    Square& sq = square_[0];
    const int delta = sq.shadowFrequency >> sq.sweepShift;
    const int result = sq.sweepDecrease ? sq.shadowFrequency - delta : sq.shadowFrequency + delta;
    if (result > 2047) sq.on = false;
    return result;
}

void APU::clockSweep() {
    Square& sq = square_[0];
    if (--sq.sweepTimer > 0) return;
    sq.sweepTimer = sq.sweepPeriod ? sq.sweepPeriod : 8;
    if (!sq.sweepEnabled || sq.sweepPeriod == 0) return;
    const int next = sweepCalculate();
    if (next <= 2047 && sq.sweepShift) {
        sq.shadowFrequency = next;
        sq.frequency = next;
        sweepCalculate();
    }
}

void APU::clockFrameSequencer() {
    // Steps 0,2,4,6: length counters. 2,6: sweep. 7: volume envelopes.
    const int step = frameSequencerStep_;
    if ((step & 1) == 0) {
        auto clockLength = [](int& length, bool enabled, bool& on) {
            if (enabled && length > 0 && --length == 0) on = false;
        };
        clockLength(square_[0].length, square_[0].lengthEnabled, square_[0].on);
        clockLength(square_[1].length, square_[1].lengthEnabled, square_[1].on);
        clockLength(wave_.length, wave_.lengthEnabled, wave_.on);
        clockLength(noise_.length, noise_.lengthEnabled, noise_.on);
    }
    if (step == 2 || step == 6) clockSweep();
    if (step == 7) {
        square_[0].envelope.clock();
        square_[1].envelope.clock();
        noise_.envelope.clock();
    }
    frameSequencerStep_ = (step + 1) & 7;
}

void APU::advanceChannels(int cycles) {
    for (int i = 0; i < 2; ++i) {
        Square& sq = square_[i];
        if (!sq.on) continue;
        sq.timer -= cycles;
        while (sq.timer <= 0) {
            sq.timer += squarePeriod(i);
            sq.step = (sq.step + 1) & 7;
        }
    }
    if (wave_.on) {
        wave_.timer -= cycles;
        while (wave_.timer <= 0) {
            wave_.timer += wavePeriod();
            wave_.position = (wave_.position + 1) % (wave_.twoBanks ? 64 : 32);
        }
    }
    if (noise_.on) {
        noise_.timer -= cycles;
        while (noise_.timer <= 0) {
            noise_.timer += noisePeriod();
            // Linear-feedback shift register: pseudo-random bits.
            const u16 bit = (noise_.lfsr ^ (noise_.lfsr >> 1)) & 1;
            noise_.lfsr = static_cast<u16>((noise_.lfsr >> 1) | (bit << 14));
            if (noise_.narrow) noise_.lfsr = static_cast<u16>((noise_.lfsr & ~0x40) | (bit << 6));
        }
    }
}

void APU::tick(int cycles) {
    frameSequencerCycles_ += cycles;
    while (frameSequencerCycles_ >= kFrameSequencerPeriod) {
        frameSequencerCycles_ -= kFrameSequencerPeriod;
        clockFrameSequencer();
    }

    // Produce output samples at kSampleRate using an exact integer ratio,
    // so there is no long-term drift between emulated and real time.
    pendingCycles_ += cycles;
    sampleAccumulator_ += static_cast<u64>(cycles) * kSampleRate;
    while (sampleAccumulator_ >= kCpuFrequency) {
        sampleAccumulator_ -= kCpuFrequency;
        advanceChannels(pendingCycles_);
        pendingCycles_ = 0;
        mixSample();
    }
}

void APU::mixSample() {
    const u16 cntL = gba_.bus.reg(0x80);
    const u16 cntH = gba_.bus.reg(0x82);
    const u16 cntX = gba_.bus.reg(0x84);
    if (!(cntX & 0x80)) {  // sound hardware switched off
        samples.push_back(0);
        samples.push_back(0);
        return;
    }

    // Each legacy channel outputs -15..+15.
    int channel[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2; ++i) {
        const Square& sq = square_[i];
        if (sq.on) channel[i] = kDutyTable[sq.duty][sq.step] ? sq.envelope.volume : -sq.envelope.volume;
    }
    if (wave_.on) {
        const int bank = wave_.twoBanks ? (wave_.bank ^ (wave_.position >> 5)) : wave_.bank;
        const int pos = wave_.position & 31;
        const u8 byte = waveRam_[bank][pos >> 1];
        const int nibble = (pos & 1) ? (byte & 15) : (byte >> 4);
        int sample = nibble * 2 - 15;
        if (wave_.force75) sample = sample * 3 / 4;
        else if (wave_.volumeCode == 0) sample = 0;
        else sample >>= (wave_.volumeCode - 1);
        channel[2] = sample;
    }
    if (noise_.on) channel[3] = (noise_.lfsr & 1) ? -noise_.envelope.volume : noise_.envelope.volume;

    int left = 0, right = 0;
    for (int i = 0; i < 4; ++i) {
        if (cntL & (0x1000 << i)) left += channel[i];
        if (cntL & (0x0100 << i)) right += channel[i];
    }
    left *= ((cntL >> 4) & 7) + 1;   // master volume 1..8
    right *= (cntL & 7) + 1;
    const int psgShift = 2 - std::min(2, cntH & 3);  // 25%, 50%, 100%
    left >>= psgShift;
    right >>= psgShift;

    // Direct Sound: signed 8-bit samples at 50% or 100% volume.
    const int dmaA = fifo_[0].current * ((cntH & 0x4) ? 4 : 2);
    const int dmaB = fifo_[1].current * ((cntH & 0x8) ? 4 : 2);
    if (cntH & 0x0200) left += dmaA;
    if (cntH & 0x0100) right += dmaA;
    if (cntH & 0x2000) left += dmaB;
    if (cntH & 0x1000) right += dmaB;

    samples.push_back(static_cast<s16>(std::clamp(left * 32, -32768, 32767)));
    samples.push_back(static_cast<s16>(std::clamp(right * 32, -32768, 32767)));
}

void APU::serialize(StateStream& s) {
    s.section("APU ");
    s.value(square_);
    s.value(wave_);
    s.value(noise_);
    s.value(fifo_);
    s.value(waveRam_);
    s.value(sampleAccumulator_);
    s.value(frameSequencerCycles_);
    s.value(frameSequencerStep_);
    s.value(pendingCycles_);
    if (!s.loading()) return;
    // Values used as array indices or shift amounts are kept in range, so a
    // damaged state can't make the mixer read outside its tables.
    for (Square& sq : square_) {
        sq.duty &= 3;
        sq.step &= 7;
    }
    wave_.bank &= 1;
    wave_.position &= 63;
    wave_.volumeCode &= 3;
    noise_.divisor &= 7;
    noise_.shift &= 15;
    for (Fifo& q : fifo_) {
        q.readPos &= 31;
        q.writePos &= 31;
        q.count = std::clamp(q.count, 0, 32);
    }
    frameSequencerStep_ &= 7;
    samples.clear();  // sound from before the load shouldn't play after it
}
