#include <cstdint>
#include <vector>

#include "bytes.hpp"
#include "test.hpp"

using tsugiki::Reader;

namespace {

uint64_t uleb(std::vector<uint8_t> bytes) {
    Reader r(bytes);
    uint64_t value = r.uleb128();
    CHECK(r.done());
    return value;
}

int64_t sleb(std::vector<uint8_t> bytes) {
    Reader r(bytes);
    int64_t value = r.sleb128();
    CHECK(r.done());
    return value;
}

}  // namespace

TEST(leb128_values) {
    CHECK_EQ(uleb({0x00}), 0u);
    CHECK_EQ(uleb({0x7f}), 127u);
    CHECK_EQ(uleb({0x80, 0x01}), 128u);
    CHECK_EQ(uleb({0xe5, 0x8e, 0x26}), 624485u);
    CHECK_EQ(uleb({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}), ~uint64_t(0));
    CHECK_EQ(sleb({0x00}), 0);
    CHECK_EQ(sleb({0x3f}), 63);
    CHECK_EQ(sleb({0x40}), -64);
    CHECK_EQ(sleb({0x7f}), -1);
    CHECK_EQ(sleb({0xc0, 0x00}), 64);
    CHECK_EQ(sleb({0xbf, 0x7f}), -65);
    CHECK_EQ(sleb({0xc0, 0xbb, 0x78}), -123456);
    CHECK_EQ(sleb({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f}), INT64_MIN);
}

TEST(reader_stops_at_the_end) {
    std::vector<uint8_t> bytes = {1, 2, 3};
    Reader r(bytes, 0x1000);
    CHECK_EQ(r.u16(), 0x0201);
    CHECK_EQ(r.position(), 0x1002u);
    bool threw = false;
    try {
        r.u16();
    } catch (const tsugiki::Error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(cstrings) {
    std::vector<uint8_t> bytes = {'h', 'i', 0, 'x'};
    Reader r(bytes);
    CHECK_EQ(r.cstring(), "hi");
    bool threw = false;
    try {
        r.cstring();
    } catch (const tsugiki::Error&) {
        threw = true;
    }
    CHECK(threw);
}
