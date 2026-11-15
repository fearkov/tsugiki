#include "eh_frame.hpp"

#include <algorithm>
#include <map>
#include <string>

#include "bytes.hpp"

namespace tsugiki::eh_frame {

namespace {

constexpr uint8_t pe_omit = 0xff;
constexpr uint8_t pe_indirect = 0x80;

struct Pointer {
    uint64_t value = 0;
    bool indirect = false;
};

// a DW_EH_PE_* encoded pointer, at r's position
// zero_is_none: a raw 0 stays 0, like libgcc and libunwind do for lsdas
std::optional<Pointer> read_pointer(Reader& r, uint8_t encoding, uint64_t datarel, bool zero_is_none = false) {
    if (encoding == pe_omit) {
        return std::nullopt;
    }
    uint64_t here = r.position();
    uint64_t value;
    switch (encoding & 0x0f) {
    case 0x00: value = r.u64(); break;
    case 0x01: value = r.uleb128(); break;
    case 0x02: value = r.u16(); break;
    case 0x03: value = r.u32(); break;
    case 0x04: value = r.u64(); break;
    case 0x09: value = uint64_t(r.sleb128()); break;
    case 0x0a: value = uint64_t(int64_t(int16_t(r.u16()))); break;
    case 0x0b: value = uint64_t(int64_t(int32_t(r.u32()))); break;
    case 0x0c: value = r.u64(); break;
    default: throw Error("eh_frame: weird pointer format " + std::to_string(encoding));
    }
    if (zero_is_none && value == 0) {
        return std::nullopt;
    }
    switch (encoding & 0x70) {
    case 0x00: break;
    case 0x10: value += here; break;
    case 0x30: value += datarel; break;
    default: throw Error("eh_frame: pointer relative to something we don't know " + std::to_string(encoding));
    }
    return Pointer{value, (encoding & pe_indirect) != 0};
}

struct Cie {
    uint8_t fde_encoding = 0;
    uint8_t lsda_encoding = pe_omit;
    bool augmented = false;
    bool signal_frame = false;
    std::optional<Pointer> personality;
};

class Walker {
public:
    explicit Walker(const elf::Library& library) : library_(library) {}

    // one CIE, by the address of its length field
    const Cie& cie(uint64_t at) {
        auto known = cies_.find(at);
        if (known != cies_.end()) {
            return known->second;
        }
        auto bytes = library_.from(at);
        if (!bytes) {
            throw Error("eh_frame: CIE outside the file");
        }
        Reader r(*bytes, at);
        uint64_t length = r.u32();
        if (length == 0xffffffff) {
            length = r.u64();
        }
        if (length > r.left()) {
            throw Error("eh_frame: CIE runs off the end");
        }
        Reader body(bytes->subspan(r.offset(), length), r.position());
        if (body.u32() != 0) {
            throw Error("eh_frame: FDE points at something that isn't a CIE");
        }
        Cie cie;
        uint8_t version = body.u8();
        std::string augmentation = body.cstring();
        if (augmentation.find("eh") != std::string::npos) {
            body.skip(8);
        }
        body.uleb128();  // code alignment
        body.sleb128();  // data alignment
        if (version == 1) {
            body.u8();
        } else {
            body.uleb128();
        }
        if (!augmentation.empty() && augmentation[0] == 'z') {
            cie.augmented = true;
            uint64_t size = body.uleb128();
            size_t end = body.offset() + size;
            for (char c : augmentation.substr(1)) {
                if (c == 'R') {
                    cie.fde_encoding = body.u8();
                } else if (c == 'P') {
                    uint8_t encoding = body.u8();
                    cie.personality = read_pointer(body, encoding, 0);
                } else if (c == 'L') {
                    cie.lsda_encoding = body.u8();
                } else if (c == 'S') {
                    cie.signal_frame = true;
                } else if (c == 'B' || c == 'G') {
                    // pac b-key, mte, no data
                } else {
                    // unknown, z says how long it all is so just stop
                    break;
                }
            }
            body.seek(end);
        }
        return cies_.emplace(at, cie).first->second;
    }

    std::vector<Fde> walk(uint64_t start) {
        std::vector<Fde> fdes;
        auto bytes = library_.from(start);
        if (!bytes) {
            throw Error("eh_frame: not in the file");
        }
        Reader r(*bytes, start);
        while (r.left() >= 4) {
            uint64_t length = r.u32();
            if (length == 0) {
                break;
            }
            if (length == 0xffffffff) {
                length = r.u64();
            }
            if (length > r.left()) {
                throw Error("eh_frame: record runs off the end");
            }
            size_t next = r.offset() + length;
            Reader body(bytes->subspan(r.offset(), length), r.position());
            uint64_t id_at = body.position();
            uint32_t id = body.u32();
            if (id != 0) {
                fdes.push_back(fde(body, cie(id_at - id)));
            }
            r.seek(next);
        }
        std::sort(fdes.begin(), fdes.end(), [](const Fde& a, const Fde& b) { return a.start < b.start; });
        return fdes;
    }

private:
    Fde fde(Reader& body, const Cie& cie) {
        Fde f;
        auto start = read_pointer(body, cie.fde_encoding, 0);
        auto size = read_pointer(body, cie.fde_encoding & 0x0f, 0);
        if (!start || !size) {
            throw Error("eh_frame: FDE without a range");
        }
        f.start = start->value;
        f.size = size->value;
        if (cie.augmented) {
            uint64_t length = body.uleb128();
            size_t end = body.offset() + length;
            if (cie.lsda_encoding != pe_omit) {
                if (auto lsda = read_pointer(body, cie.lsda_encoding, 0, true)) {
                    f.lsda = lsda->value;
                }
            }
            body.seek(end);
        }
        if (cie.personality) {
            f.personality = cie.personality->value;
            f.personality_indirect = cie.personality->indirect;
        }
        f.signal_frame = cie.signal_frame;
        return f;
    }

    const elf::Library& library_;
    std::map<uint64_t, Cie> cies_;
};

}  // namespace

std::vector<Fde> read(const elf::Library& library) {
    // TODO: .eh_frame without a hdr, via the section headers
    auto header = library.segment(elf::segment::gnu_eh_frame);
    if (!header) {
        return {};
    }
    auto bytes = library.at(header->vaddr, header->filesz);
    if (!bytes) {
        throw Error("eh_frame_hdr not in the file");
    }
    Reader r(*bytes, header->vaddr);
    if (r.u8() != 1) {
        throw Error("eh_frame_hdr: unknown version");
    }
    uint8_t frame_encoding = r.u8();
    r.u8();  // fde count encoding
    r.u8();  // table encoding
    auto frame = read_pointer(r, frame_encoding, header->vaddr);
    if (!frame || frame->indirect) {
        throw Error("eh_frame_hdr: no eh_frame pointer");
    }
    return Walker(library).walk(frame->value);
}

}  // namespace tsugiki::eh_frame
