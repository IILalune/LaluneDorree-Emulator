// state.hpp - Copying the whole console to and from a block of bytes (a "save state").
//
// Every component has a `serialize(StateStream&)` method that lists the
// fields making up its state. The same method both saves and loads: when
// saving, each field is appended to a byte buffer; when loading, it is read
// back from one. Listing the fields once means saving and loading can never
// get out of step with each other.
//
// The layout is the in-memory layout of each field, so it only has to make
// sense to this program. Bump GBA::kStateVersion whenever a serialized field
// is added, removed or changes type: older states are then refused instead
// of being misread.
#pragma once
#include <cstddef>
#include <type_traits>
#include <vector>
#include "common.hpp"

class StateStream {
public:
    // Saving: fields are appended to `out`.
    explicit StateStream(std::vector<u8>& out) : out_(&out) {}
    // Loading: fields are read from `data`. Reading past the end stops the
    // load (ok() turns false) and leaves the remaining fields untouched.
    StateStream(const u8* data, std::size_t size) : in_(data), size_(size) {}

    bool loading() const { return out_ == nullptr; }
    bool ok() const { return ok_; }
    // True once a load has used up every byte (extra bytes mean a mismatch).
    bool atEnd() const { return pos_ == size_; }
    void fail() { ok_ = false; }

    // Any value that can be copied byte for byte: integers, bools, plain
    // structs, and std::array or C arrays of them.
    template <typename T>
    void value(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "only plain data can be copied byte for byte");
        bytes(&v, sizeof(T));
    }

    void bytes(void* data, std::size_t size);

    // A byte vector whose length can change, stored with its length first.
    // Loading refuses lengths over `maxSize`.
    void vector(std::vector<u8>& v, std::size_t maxSize);

    // A 4-letter tag between components. If a load finds a different tag,
    // the data doesn't line up with this program's layout and the load fails.
    void section(const char (&tag)[5]);

private:
    std::vector<u8>* out_ = nullptr;
    const u8* in_ = nullptr;
    std::size_t size_ = 0, pos_ = 0;
    bool ok_ = true;
};

// A fast 64-bit fingerprint of a block of bytes (FNV-1a over 8-byte words).
// Used to tell ROMs apart and to spot damaged save state files; it is not
// meant to resist deliberate tampering.
u64 hashBytes(const u8* data, std::size_t size);
