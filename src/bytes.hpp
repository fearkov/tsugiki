// little-endian reading for elf and dwarf stuff
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace tsugiki {

static_assert(std::endian::native == std::endian::little, "only little-endian hosts");

// bad input
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// cursor over bytes, throws if we run off the end
// base = address of the first byte, for pc-relative stuff
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes, uint64_t base = 0) : bytes_(bytes), base_(base) {}

    uint64_t position() const { return base_ + offset_; }
    size_t offset() const { return offset_; }
    size_t left() const { return bytes_.size() - offset_; }
    bool done() const { return offset_ >= bytes_.size(); }

    void seek(size_t offset) {
        if (offset > bytes_.size()) {
            throw Error("seek past the end");
        }
        offset_ = offset;
    }

    void skip(size_t count) {
        need(count);
        offset_ += count;
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    T get() {
        need(sizeof(T));
        T value;
        std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }

    uint64_t uleb128() {
        uint64_t result = 0;
        unsigned shift = 0;
        while (true) {
            uint8_t byte = u8();
            if (shift < 64) {
                result |= uint64_t(byte & 0x7f) << shift;
            }
            shift += 7;
            if ((byte & 0x80) == 0) {
                return result;
            }
        }
    }

    int64_t sleb128() {
        uint64_t result = 0;
        unsigned shift = 0;
        uint8_t byte;
        do {
            byte = u8();
            if (shift < 64) {
                result |= uint64_t(byte & 0x7f) << shift;
            }
            shift += 7;
        } while (byte & 0x80);
        // sign bit is bit 6 of the last byte
        if (shift < 64 && (byte & 0x40)) {
            result |= ~uint64_t(0) << shift;
        }
        return int64_t(result);
    }

    // zero-terminated, skips the zero
    std::string cstring() {
        const void* end = std::memchr(bytes_.data() + offset_, 0, left());
        if (end == nullptr) {
            throw Error("string runs off the end");
        }
        size_t length = static_cast<const uint8_t*>(end) - (bytes_.data() + offset_);
        std::string text(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length + 1;
        return text;
    }

private:
    void need(size_t count) const {
        if (left() < count) {
            throw Error("read past the end");
        }
    }

    std::span<const uint8_t> bytes_;
    uint64_t base_;
    size_t offset_ = 0;
};

}  // namespace tsugiki
