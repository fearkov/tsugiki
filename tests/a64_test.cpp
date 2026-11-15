#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "a64/decoder.hpp"
#include "test.hpp"

using namespace tsugiki;

namespace {

std::string run(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    CHECK(pipe != nullptr);
    std::string output;
    char buffer[65536];
    while (size_t got = fread(buffer, 1, sizeof buffer, pipe)) {
        output.append(buffer, got);
    }
    pclose(pipe);
    return output;
}

// what llvm calls the words, "<unknown>" if nothing
std::vector<std::string> llvm_mnemonics(const std::vector<uint32_t>& words) {
    std::string dir = std::string(SAMPLE_DIR);
    std::string source = dir + "/words.s", object = dir + "/words.o";
    {
        std::ofstream out(source);
        out << ".text\n";
        for (uint32_t w : words) {
            out << ".inst 0x" << std::hex << w << "\n";
        }
    }
    std::string assemble = std::string(CLANG) + " --target=aarch64-linux-android -c " + source + " -o " + object;
    CHECK(std::system(assemble.c_str()) == 0);
    // a real phone's features, not every extension llvm knows
    std::istringstream lines(run(std::string(LLVM_OBJDUMP) + " -d -M no-aliases --mcpu=cortex-x3 --mattr=+aes,+sha2 " + object));
    std::vector<std::string> names;
    std::string line;
    while (std::getline(lines, line)) {
        // "      14: f9400420     	ldr	x0, [x1, #0x8]"
        size_t colon = line.find(':');
        if (colon == std::string::npos || line.find('\t') == std::string::npos || line.find('<') == 0) {
            continue;
        }
        std::string head = line.substr(0, colon);
        if (head.find_first_not_of(" 0123456789abcdef") != std::string::npos || head.find_first_not_of(' ') == std::string::npos) {
            continue;
        }
        size_t tab = line.find('\t', colon);
        std::string rest = line.substr(tab + 1);
        std::string name = rest.substr(0, rest.find_first_of("\t "));
        names.push_back(name);
    }
    return names;
}

// llvm still prints these aliases with -M no-aliases
const std::map<std::string, std::set<std::string>> aliases = {
    {"ubfm", {"ubfx", "ubfiz", "lsl", "lsr", "uxtb", "uxth"}},
    {"sbfm", {"sbfx", "sbfiz", "asr", "sxtb", "sxth", "sxtw"}},
    {"bfm", {"bfi", "bfxil", "bfc"}},
    {"extr", {"ror"}},
    {"lslv", {"lsl"}},
    {"lsrv", {"lsr"}},
    {"asrv", {"asr"}},
    {"rorv", {"ror"}},
    {"movn", {"mov"}},
    {"movz", {"mov"}},
    {"orr", {"mov"}},
    {"prfm", {"rprfm"}},
    {"dup", {"mov"}},
};

bool accepts(const std::string& ours, const std::string& theirs, a64::Op op) {
    if (ours == theirs) {
        return true;
    }
    auto alias = aliases.find(ours);
    if (alias != aliases.end() && alias->second.contains(theirs)) {
        return true;
    }
    // named hints, barriers, system ops, all the same to us
    switch (op) {
    case a64::Op::hint:
    case a64::Op::dsb:
    case a64::Op::sys:
    case a64::Op::sysl:
    case a64::Op::msr_imm:
    case a64::Op::msr_reg:
    case a64::Op::mrs:
        return theirs != "<unknown>";
    default:
        break;
    }
    // b.cs and b.hs are the same thing
    if (ours.starts_with("b.") || ours.starts_with("bc.")) {
        std::map<std::string, std::string> same = {{"hs", "cs"}, {"lo", "cc"}};
        size_t dot = ours.find('.');
        auto other = same.find(ours.substr(dot + 1));
        return other != same.end() && theirs == ours.substr(0, dot + 1) + other->second;
    }
    return false;
}

}  // namespace

TEST(a64_decoder_matches_llvm) {
    if (std::string(LLVM_OBJDUMP).ends_with("NOTFOUND")) {
        return;
    }
    std::mt19937 random(1234);
    std::vector<uint32_t> words;
    // each pattern with random free bits
    for (const a64::Encoding& e : a64::encodings()) {
        for (int i = 0; i < 300; ++i) {
            words.push_back(e.value | (uint32_t(random()) & ~e.mask));
        }
    }
    // plus plain random ones
    for (int i = 0; i < 40000; ++i) {
        words.push_back(uint32_t(random()));
    }
    auto theirs = llvm_mnemonics(words);
    CHECK_EQ(theirs.size(), words.size());

    // what llvm knows that we skip on purpose, mte and friends
    const std::set<std::string> skipped = {"stgp", "addg", "subg", "subp", "subps", "irg", "gmi", "stg", "stzg", "st2g", "stz2g",
                                           "ldg", "stgm", "stzgm", "ldgm", "hvc", "smc", "dcps1", "dcps2", "dcps3", "eret", "eretaa",
                                           "eretab", "drps", "tcancel", "tstart", "ttest", "tcommit", "rmif", "setf8", "setf16",
                                           "cfinv", "xaflag", "axflag", "wfet", "wfit", "udf"};
    std::map<std::string, std::vector<std::string>> wrong;
    int checked = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        a64::Instruction in = a64::decode(words[i]);
        const std::string& name = theirs[i];
        bool ok;
        if (in.op == a64::Op::simd) {
            // fp16 vectors and such, not ours yet
            ok = true;
        } else if (in.valid()) {
            ok = accepts(a64::mnemonic(in), name, in.op);
            // newer than the cpu we ask llvm about: hbc, cssc
            std::string ours = a64::mnemonic(in);
            bool newer = ours.starts_with("bc.") || ours == "smax" || ours == "smin" || ours == "umax" || ours == "umin" ||
                         ours == "abs" || ours == "cnt" || ours == "ctz";
            ok = ok || (newer && name == "<unknown>");
        } else if (in.op == a64::Op::simd) {
            // fp16 vectors and such, not ours yet
            ok = true;
        } else if ((words[i] >> 23) == 0b110101010) {
            // odd system encodings llvm still names, no app has them
            ok = true;
        } else {
            // op0 00xx is sve, sme and unallocated, not ours yet
            bool sve = ((words[i] >> 27) & 3) == 0;
            ok = name == "<unknown>" || sve || skipped.contains(name);
        }
        ++checked;
        if (!ok) {
            char text[64];
            std::snprintf(text, sizeof text, "%08x ours %s llvm %s", words[i], in.valid() ? a64::mnemonic(in).c_str() : "invalid", name.c_str());
            auto& list = wrong[in.valid() ? a64::mnemonic(in) : "invalid:" + name];
            if (list.size() < 3) {
                list.push_back(text);
            }
        }
    }
    if (!wrong.empty()) {
        std::string message = std::to_string(wrong.size()) + " kinds of mismatch:";
        for (auto& [kind, examples] : wrong) {
            message += "\n       " + kind + ":";
            for (auto& e : examples) {
                message += " [" + e + "]";
            }
        }
        throw test::Failure{message};
    }
    CHECK(checked > 50000);
}
