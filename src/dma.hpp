// dma.hpp - Direct Memory Access controller (4 channels).
//
// DMA copies blocks of memory without the CPU. Games use it to upload
// graphics, to update scroll registers every scanline (HBlank DMA), to feed
// the sound FIFOs, and to talk to the EEPROM save chip.
#pragma once

#include "common.hpp"

class GBA;
class StateStream;

class DMA {
public:
    explicit DMA(GBA& gba);
    void reset();

    void writeControl(int channel, u16 oldValue, u16 value);
    void onVBlank();
    void onHBlank();
    void onFifoRequest(int fifo);  // fifo 0 = A, 1 = B
    void serialize(StateStream& s);  // see state.hpp

private:
    struct Channel {
        u32 src = 0, dst = 0, count = 0;  // internal (latched) state
    };

    GBA& gba_;
    Channel channels_[4];

    void transfer(int channel);
    u16 control(int channel) const;
};
