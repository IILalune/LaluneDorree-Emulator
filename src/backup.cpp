#include "backup.hpp"

#include <algorithm>

#include "state.hpp"

void Eeprom::reset() {
    state_ = State::Receiving;
    bits_.clear();
    sendBuffer_ = 0;
    sendPos_ = 0;
}

void Eeprom::detectSizeFromDma(u32 count) {
    // Read request: 2 command bits + address + 1 stop bit.
    // Write request: 2 command bits + address + 64 data bits + 1 stop bit.
    if (count == 9 || count == 73) addrBits_ = 6;
    else if (count == 17 || count == 81) addrBits_ = 14;
}

u32 Eeprom::bitsToNumber(std::size_t start, int count) const {
    u32 value = 0;
    for (int i = 0; i < count; ++i) value = (value << 1) | bits_[start + i];
    return value;
}

void Eeprom::writeBit(u16 value) {
    if (state_ == State::Sending) {
        // A new request aborts any half-finished read.
        state_ = State::Receiving;
        bits_.clear();
    }
    bits_.push_back(value & 1);

    const int addrBits = addrBits_ ? addrBits_ : 14;
    if (bits_.size() < 2) return;

    const u32 command = (bits_[0] << 1) | bits_[1];
    // `>=` rather than `==`: the address size can be detected in the middle of
    // a request, and the buffer must never grow without limit.
    if (command == 3) {  // read request
        if (bits_.size() >= static_cast<std::size_t>(2 + addrBits + 1)) {
            const u32 block = bitsToNumber(2, addrBits) & 0x3FF;
            const u32 offset = (block * 8) % data.size();
            sendBuffer_ = 0;
            for (int i = 0; i < 8; ++i) sendBuffer_ = (sendBuffer_ << 8) | data[offset + i];
            sendPos_ = 0;
            state_ = State::Sending;
            bits_.clear();
        }
    } else if (command == 2) {  // write request
        if (bits_.size() >= static_cast<std::size_t>(2 + addrBits + 64 + 1)) {
            const u32 block = bitsToNumber(2, addrBits) & 0x3FF;
            const u32 offset = (block * 8) % data.size();
            for (int i = 0; i < 8; ++i) {
                data[offset + i] = static_cast<u8>(bitsToNumber(2 + addrBits + i * 8, 8));
            }
            dirty = true;
            bits_.clear();
        }
    } else {
        bits_.clear();  // invalid command, start over
    }
}

u16 Eeprom::readBit() {
    if (state_ != State::Sending) return 1;  // "ready"
    u16 bit = 0;
    if (sendPos_ >= 4) {  // the first 4 bits of a response are junk
        bit = static_cast<u16>((sendBuffer_ >> (63 - (sendPos_ - 4))) & 1);
    }
    if (++sendPos_ == 68) state_ = State::Receiving;
    return bit;
}

void Eeprom::serialize(StateStream& s) {
    s.section("EEPR");
    s.value(state_);
    s.vector(bits_, 2 + 14 + 64 + 1);  // the longest request
    s.value(sendBuffer_);
    s.value(sendPos_);
    if (s.loading()) {
        if (state_ != State::Receiving && state_ != State::Sending) state_ = State::Receiving;
        sendPos_ = std::clamp(sendPos_, 0, 68);
    }
}
