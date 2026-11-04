#include "dma.hpp"

#include "gba.hpp"
#include "state.hpp"

namespace {
constexpr u32 kBase[4] = {0xB0, 0xBC, 0xC8, 0xD4};  // register block per channel
}

DMA::DMA(GBA& gba) : gba_(gba) {}

void DMA::reset() {
    for (auto& c : channels_) c = Channel{};
}

u16 DMA::control(int ch) const { return gba_.bus.io[(kBase[ch] + 10) >> 1]; }

void DMA::writeControl(int ch, u16 oldValue, u16 value) {
    // A channel only latches its addresses when the enable bit goes 0 -> 1.
    if (!(value & 0x8000) || (oldValue & 0x8000)) return;

    Bus& bus = gba_.bus;
    const u32 base = kBase[ch];

    Channel& c = channels_[ch];
    c.src = (bus.reg(base) | (static_cast<u32>(bus.reg(base + 2)) << 16)) & (ch == 0 ? 0x07FFFFFF : 0x0FFFFFFF);
    c.dst = (bus.reg(base + 4) | (static_cast<u32>(bus.reg(base + 6)) << 16)) & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
    c.count = bus.reg(base + 8) & (ch == 3 ? 0xFFFF : 0x3FFF);
    if (c.count == 0) c.count = ch == 3 ? 0x10000 : 0x4000;

    if (((value >> 12) & 3) == 0) transfer(ch);  // "immediate" timing
}

void DMA::onVBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const u16 cnt = control(ch);
        if ((cnt & 0x8000) && ((cnt >> 12) & 3) == 1) transfer(ch);
    }
}

void DMA::onHBlank() {
    for (int ch = 0; ch < 4; ++ch) {
        const u16 cnt = control(ch);
        if ((cnt & 0x8000) && ((cnt >> 12) & 3) == 2) transfer(ch);
    }
}

void DMA::onFifoRequest(int fifo) {
    const u32 fifoAddr = fifo == 0 ? 0x040000A0 : 0x040000A4;
    for (int ch = 1; ch <= 2; ++ch) {
        const u16 cnt = control(ch);
        if ((cnt & 0x8000) && ((cnt >> 12) & 3) == 3 && channels_[ch].dst == fifoAddr) transfer(ch);
    }
}

void DMA::transfer(int ch) {
    Bus& bus = gba_.bus;
    Channel& c = channels_[ch];
    u16 cnt = control(ch);
    const u32 timing = (cnt >> 12) & 3;
    const bool fifoMode = timing == 3 && (ch == 1 || ch == 2);

    bool word = cnt & 0x400;
    u32 count = c.count;
    const u32 dstControl = (cnt >> 5) & 3;
    const u32 srcControl = (cnt >> 7) & 3;
    if (fifoMode) { word = true; count = 4; }

    // The EEPROM's size can only be guessed from how many bits get sent.
    if (ch == 3 && ((c.dst >> 24) == 0x0D || (c.src >> 24) == 0x0D)) {
        bus.eeprom.detectSizeFromDma(count);
    }

    const s32 step = word ? 4 : 2;
    auto delta = [step](u32 control) -> s32 {
        switch (control) {
        case 1: return -step;  // decrement
        case 2: return 0;      // fixed
        default: return step;  // increment (3 = increment + reload for dst)
        }
    };
    const s32 srcStep = delta(srcControl);
    const s32 dstStep = fifoMode ? 0 : delta(dstControl);

    for (u32 i = 0; i < count; ++i) {
        if (word) bus.write32(c.dst & ~3u, bus.read32(c.src & ~3u));
        else bus.write16(c.dst & ~1u, bus.read16(c.src & ~1u));
        c.src += srcStep;
        c.dst += dstStep;
    }
    bus.cycles += 2;

    if (cnt & 0x4000) gba_.raiseIrq(IrqDma0 + ch);

    const u32 base = kBase[ch];
    if ((cnt & 0x200) && timing != 0) {  // repeat: re-arm for the next trigger
        if (!fifoMode) {
            c.count = bus.reg(base + 8) & (ch == 3 ? 0xFFFF : 0x3FFF);
            if (c.count == 0) c.count = ch == 3 ? 0x10000 : 0x4000;
        }
        if (dstControl == 3) {
            c.dst = (bus.reg(base + 4) | (static_cast<u32>(bus.reg(base + 6)) << 16)) &
                    (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        }
    } else {
        bus.reg(base + 10) = cnt & ~0x8000;  // one-shot transfer: disable channel
    }
}

void DMA::serialize(StateStream& s) {
    s.section("DMA ");
    s.value(channels_);
}
