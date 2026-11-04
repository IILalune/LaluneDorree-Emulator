#include "state.hpp"

void StateStream::bytes(void* data, std::size_t size) {
    if (!ok_) return;
    if (!loading()) {
        const u8* p = static_cast<const u8*>(data);
        out_->insert(out_->end(), p, p + size);
        return;
    }
    if (size > size_ - pos_) {
        ok_ = false;
        return;
    }
    std::memcpy(data, in_ + pos_, size);
    pos_ += size;
}

void StateStream::vector(std::vector<u8>& v, std::size_t maxSize) {
    u32 length = static_cast<u32>(v.size());
    value(length);
    if (!ok_) return;
    if (loading()) {
        if (length > maxSize || length > size_ - pos_) {
            ok_ = false;
            return;
        }
        v.assign(in_ + pos_, in_ + pos_ + length);
        pos_ += length;
    } else {
        bytes(v.data(), v.size());
    }
}

void StateStream::section(const char (&tag)[5]) {
    u32 expected = 0;
    std::memcpy(&expected, tag, 4);
    u32 found = expected;
    value(found);
    if (found != expected) ok_ = false;
}

u64 hashBytes(const u8* data, std::size_t size) {
    constexpr u64 kPrime = 0x100000001B3ull;
    u64 hash = 0xCBF29CE484222325ull ^ size;
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) hash = (hash ^ loadLE<u64>(data, static_cast<u32>(i))) * kPrime;
    for (; i < size; ++i) hash = (hash ^ data[i]) * kPrime;
    // Mix the last bits through so nearby inputs don't give nearby hashes.
    hash ^= hash >> 33;
    hash *= 0xFF51AFD7ED558CCDull;
    hash ^= hash >> 33;
    return hash;
}
