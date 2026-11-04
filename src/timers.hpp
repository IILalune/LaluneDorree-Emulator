// timers.hpp - The four 16-bit hardware timers.
//
// Each timer counts up at the CPU clock divided by 1, 64, 256 or 1024 and
// fires on overflow (reloading a start value). Timers 0 and 1 also clock the
// digital sound FIFOs, and a timer can "cascade", counting overflows of the
// previous one.
#pragma once

#include "common.hpp"

class GBA;
class StateStream;

class Timers {
public:
    explicit Timers(GBA& gba);
    void reset();
    void tick(int cycles);
    int cyclesUntilOverflow() const;

    u16 readCounter(int i) const { return static_cast<u16>(timers_[i].counter); }
    void writeReload(int i, u16 value) { timers_[i].reload = value; }
    void writeControl(int i, u16 value);
    void serialize(StateStream& s);  // see state.hpp

private:
    struct Timer {
        u32 counter = 0;
        u16 reload = 0;
        u16 control = 0;
        u32 subCycles = 0;  // cycles accumulated toward the next prescaled tick
        bool enabled() const { return control & 0x80; }
        bool cascade() const { return control & 0x04; }
        int shift() const {
            static constexpr int kShifts[4] = {0, 6, 8, 10};
            return kShifts[control & 3];
        }
    };

    GBA& gba_;
    Timer timers_[4];

    void increment(int i, u32 amount);
    void overflow(int i);
};
