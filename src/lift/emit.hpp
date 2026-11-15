// little c++ text helpers shared by the lifters
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace tsugiki::lift::text {

inline int64_t sign_extend(uint64_t value, int bits) {
    uint64_t sign = uint64_t(1) << (bits - 1);
    return int64_t((value ^ sign) - sign);
}

inline std::string hex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof text, "0x%llxull", static_cast<unsigned long long>(value));
    return text;
}

inline std::string name(uint64_t address) {
    char text[32];
    std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(address));
    return text;
}

inline std::string x(int n) {
    return "s->x[" + std::to_string(n) + "]";
}

// a register as a value, 31 is zr or sp
inline std::string get(int n, bool wide, bool sp = false) {
    if (n == 31 && !sp) {
        return wide ? "(uint64_t)0" : "(uint32_t)0";
    }
    return wide ? x(n) : "(uint32_t)" + x(n);
}

// assignment, 31 drops the value unless it's sp, w zero-extends
inline std::string set(int n, bool wide, const std::string& value, bool sp = false) {
    if (n == 31 && !sp) {
        return "(void)(" + value + ");";
    }
    return x(n) + " = " + (wide ? "(uint64_t)(" + value + ")" : "(uint64_t)(uint32_t)(" + value + ")") + ";";
}

inline std::string condition(unsigned code) {
    switch (code & 15) {
    case 0: return "fl.zf";
    case 1: return "!fl.zf";
    case 2: return "fl.cf";
    case 3: return "!fl.cf";
    case 4: return "fl.nf";
    case 5: return "!fl.nf";
    case 6: return "fl.vf";
    case 7: return "!fl.vf";
    case 8: return "(fl.cf && !fl.zf)";
    case 9: return "!(fl.cf && !fl.zf)";
    case 10: return "(fl.nf == fl.vf)";
    case 11: return "(fl.nf != fl.vf)";
    case 12: return "(!fl.zf && fl.nf == fl.vf)";
    case 13: return "!(!fl.zf && fl.nf == fl.vf)";
    default: return "1";
    }
}

}  // namespace tsugiki::lift::text
