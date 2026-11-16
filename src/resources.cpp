#include "resources.hpp"

#include "bytes.hpp"

namespace tsugiki::resources {

namespace {

constexpr uint16_t string_pool = 0x0001, table = 0x0002, xml = 0x0003, start_element = 0x0102;
constexpr uint32_t utf8_flag = 0x100;

uint8_t u8(const std::vector<uint8_t>& b, size_t at) {
    if (at >= b.size()) {
        throw Error("resources: cut short");
    }
    return b[at];
}

uint16_t u16(const std::vector<uint8_t>& b, size_t at) {
    return uint16_t(u8(b, at) | u8(b, at + 1) << 8);
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(u16(b, at)) | uint32_t(u16(b, at + 2)) << 16;
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + size_t(i)] = uint8_t(v >> (8 * i));
    }
}

// a string pool chunk: every string's bytes as stored, and its text
struct Pool {
    size_t at = 0;
    uint16_t header = 0;
    uint32_t size = 0, count = 0, styles = 0, flags = 0, strings_start = 0, styles_start = 0;
    std::vector<std::vector<uint8_t>> raw;
    std::vector<std::string> text;
    bool utf8() const { return flags & utf8_flag; }
};

Pool read_pool(const std::vector<uint8_t>& b, size_t at) {
    Pool p;
    p.at = at;
    if (u16(b, at) != string_pool) {
        throw Error("resources: no string pool where it should be");
    }
    p.header = u16(b, at + 2);
    p.size = u32(b, at + 4);
    p.count = u32(b, at + 8);
    p.styles = u32(b, at + 12);
    p.flags = u32(b, at + 16);
    p.strings_start = u32(b, at + 20);
    p.styles_start = u32(b, at + 24);
    if (at + p.size > b.size() || p.header < 28) {
        throw Error("resources: broken string pool");
    }
    for (uint32_t i = 0; i < p.count; ++i) {
        size_t s = at + p.strings_start + u32(b, at + p.header + 4 * size_t(i));
        size_t start = s;
        std::string text;
        if (p.utf8()) {
            // utf-16 length then byte length, one byte each or two with the top bit set
            auto length = [&] {
                size_t n = u8(b, s++);
                return n & 0x80 ? (n & 0x7f) << 8 | u8(b, s++) : n;
            };
            length();
            size_t n = length();
            for (size_t k = 0; k < n; ++k) {
                text += char(u8(b, s + k));
            }
            s += n + 1;
        } else {
            size_t n = u16(b, s);
            s += 2;
            if (n & 0x8000) {
                n = (n & 0x7fff) << 16 | u16(b, s);
                s += 2;
            }
            // only ascii matters here
            for (size_t k = 0; k < n; ++k) {
                uint16_t c = u16(b, s + 2 * k);
                text += c < 0x80 ? char(c) : '?';
            }
            s += 2 * n + 2;
        }
        p.raw.emplace_back(b.begin() + long(start), b.begin() + long(s));
        p.text.push_back(std::move(text));
    }
    return p;
}

// an ascii string the way the pool stores them
std::vector<uint8_t> encode(const std::string& text, bool utf8) {
    std::vector<uint8_t> out;
    size_t n = text.size();
    if (utf8) {
        auto length = [&](size_t v) {
            if (v > 0x7f) {
                out.push_back(uint8_t(0x80 | v >> 8));
            }
            out.push_back(uint8_t(v));
        };
        length(n);
        length(n);
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
        return out;
    }
    auto put = [&](uint16_t v) {
        out.push_back(uint8_t(v));
        out.push_back(uint8_t(v >> 8));
    };
    if (n > 0x7fff) {
        put(uint16_t(0x8000 | n >> 16));
    }
    put(uint16_t(n));
    for (char c : text) {
        put(uint8_t(c));
    }
    put(0);
    return out;
}

// the pool chunk built again from its strings, styles kept as they were
std::vector<uint8_t> write_pool(const std::vector<uint8_t>& b, const Pool& p) {
    std::vector<uint8_t> out(b.begin() + long(p.at), b.begin() + long(p.at + p.header));
    std::vector<uint8_t> strings;
    for (const auto& r : p.raw) {
        size_t offset = strings.size();
        for (int i = 0; i < 4; ++i) {
            out.push_back(uint8_t(offset >> (8 * i)));
        }
        strings.insert(strings.end(), r.begin(), r.end());
    }
    while (strings.size() % 4) {
        strings.push_back(0);
    }
    size_t style_offsets = p.at + p.header + 4 * size_t(p.count);
    out.insert(out.end(), b.begin() + long(style_offsets), b.begin() + long(style_offsets + 4 * size_t(p.styles)));
    uint32_t strings_start = uint32_t(out.size());
    out.insert(out.end(), strings.begin(), strings.end());
    uint32_t styles_start = 0;
    if (p.styles && p.styles_start) {
        styles_start = uint32_t(out.size());
        out.insert(out.end(), b.begin() + long(p.at + p.styles_start), b.begin() + long(p.at + p.size));
    }
    put32(out, 4, uint32_t(out.size()));
    put32(out, 20, strings_start);
    put32(out, 24, styles_start);
    return out;
}

}  // namespace

std::optional<std::vector<uint8_t>> retarget_qt_libs(const std::vector<uint8_t>& arsc) {
    if (arsc.size() < 12 || u16(arsc, 0) != table) {
        return std::nullopt;
    }
    // the global pool comes right after the table header
    size_t at = u16(arsc, 2);
    Pool p = read_pool(arsc, at);
    const std::string from = "arm64-v8a;";
    bool changed = false;
    for (size_t i = 0; i < p.text.size(); ++i) {
        const std::string& t = p.text[i];
        if (t.starts_with(from) && t.find('?') == std::string::npos) {
            p.raw[i] = encode("x86_64;" + t.substr(from.size()), p.utf8());
            changed = true;
        }
    }
    if (!changed) {
        return std::nullopt;
    }
    std::vector<uint8_t> pool = write_pool(arsc, p);
    std::vector<uint8_t> out(arsc.begin(), arsc.begin() + long(at));
    out.insert(out.end(), pool.begin(), pool.end());
    out.insert(out.end(), arsc.begin() + long(at + p.size), arsc.end());
    put32(out, 4, uint32_t(out.size()));
    return out;
}

std::optional<std::string> manifest_meta(const std::vector<uint8_t>& m, const std::string& name) {
    if (m.size() < 8 || u16(m, 0) != xml) {
        return std::nullopt;
    }
    std::optional<Pool> pool;
    auto text = [&](uint32_t i) { return i < pool->text.size() ? pool->text[i] : std::string(); };
    for (size_t at = u16(m, 2); at + 8 <= m.size();) {
        uint16_t type = u16(m, at), header = u16(m, at + 2);
        uint32_t size = u32(m, at + 4);
        if (size < 8 || at + size > m.size()) {
            break;
        }
        if (type == string_pool) {
            pool = read_pool(m, at);
        } else if (type == start_element && pool) {
            size_t ext = at + header;
            if (text(u32(m, ext + 4)) == "meta-data") {
                uint16_t first = u16(m, ext + 8), each = u16(m, ext + 10), count = u16(m, ext + 12);
                std::optional<std::string> key, value;
                for (uint16_t k = 0; k < count; ++k) {
                    size_t a = ext + first + size_t(k) * each;
                    std::string attribute = text(u32(m, a + 4));
                    uint32_t raw = u32(m, a + 8);
                    std::optional<std::string> v;
                    if (raw != 0xffffffff) {
                        v = text(raw);
                    } else if (u8(m, a + 15) == 0x03) {
                        v = text(u32(m, a + 16));
                    }
                    if (attribute == "name") {
                        key = v;
                    } else if (attribute == "value") {
                        value = v;
                    }
                }
                if (key == name && value) {
                    return value;
                }
            }
        }
        at += size;
    }
    return std::nullopt;
}

}  // namespace tsugiki::resources
