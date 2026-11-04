#include "timers.hpp"

#include <climits>

#include "gba.hpp"
#include "state.hpp"

Timers::Timers(GBA& gba) : gba_(gba) {}

void Timers::reset() {
    for (auto& t : timers_) t = Timer{};
}

void Timers::writeControl(int i, u16 value) {
    Timer& t = timers_[i];
    const bool wasEnabled = t.enabled();
    t.control = value;
    if (!wasEnabled && t.enabled()) {  // starting a timer reloads it
        t.counter = t.reload;
        t.subCycles = 0;
    }
}

void Timers::tick(int cycles) {
    for (int i = 0; i < 4; ++i) {
        Timer& t = timers_[i];
        if (!t.enabled() || (i > 0 && t.cascade())) continue;
        t.subCycles += static_cast<u32>(cycles);
        const int shift = t.shift();
        const u32 ticks = t.subCycles >> shift;
        if (ticks == 0) continue;
        t.subCycles &= (1u << shift) - 1;
        increment(i, ticks);
    }
}

void Timers::increment(int i, u32 amount) {
    Timer& t = timers_[i];
    t.counter += amount;
    while (t.counter > 0xFFFF) {
        t.counter = t.counter - 0x10000 + t.reload;
        overflow(i);
    }
}

void Timers::overflow(int i) {
    if (timers_[i].control & 0x40) gba_.raiseIrq(IrqTimer0 + i);
    if (i < 2) gba_.apu.onTimerOverflow(i);
    if (i < 3) {
        Timer& next = timers_[i + 1];
        if (next.enabled() && next.cascade()) increment(i + 1, 1);
    }
}

int Timers::cyclesUntilOverflow() const {
    int best = INT_MAX;
    for (int i = 0; i < 4; ++i) {
        const Timer& t = timers_[i];
        if (!t.enabled() || (i > 0 && t.cascade())) continue;
        // Only matters if something listens for the overflow.
        const s64 cycles = (static_cast<s64>(0x10000 - t.counter) << t.shift()) - t.subCycles;
        if (cycles < best) best = static_cast<int>(cycles < 1 ? 1 : cycles);
    }
    return best;
}

void Timers::serialize(StateStream& s) {
    s.section("TIMR");
    s.value(timers_);
}
