// apks are zips, libs live in lib/<abi>/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct zip;

namespace tsugiki::apk {

class Apk {
public:
    explicit Apk(const std::string& path);
    ~Apk();
    Apk(const Apk&) = delete;
    Apk& operator=(const Apk&) = delete;

    std::vector<std::string> names() const;
    std::vector<uint8_t> read(const std::string& name) const;

    // abis with at least one lib, like arm64-v8a
    std::vector<std::string> abis() const;
    // full entry names of an abi's .so files
    std::vector<std::string> libraries(const std::string& abi) const;

private:
    std::string path_;
    zip* zip_ = nullptr;
};

// "PK\3\4" at the start
bool looks_like_zip(const std::vector<uint8_t>& head);

}  // namespace tsugiki::apk
