#include "apk.hpp"

#include <zip.h>

#include <algorithm>
#include <cstring>

#include "bytes.hpp"

namespace tsugiki::apk {

Apk::Apk(const std::string& path) : path_(path) {
    int error = 0;
    zip_ = zip_open(path.c_str(), ZIP_RDONLY, &error);
    if (zip_ == nullptr) {
        zip_error_t e;
        zip_error_init_with_code(&e, error);
        std::string message = path + ": " + zip_error_strerror(&e);
        zip_error_fini(&e);
        throw Error(message);
    }
}

Apk::~Apk() {
    if (zip_) {
        zip_discard(zip_);
    }
}

std::vector<std::string> Apk::names() const {
    std::vector<std::string> names;
    zip_int64_t count = zip_get_num_entries(zip_, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        if (const char* name = zip_get_name(zip_, zip_uint64_t(i), ZIP_FL_ENC_RAW)) {
            names.emplace_back(name);
        }
    }
    return names;
}

std::vector<uint8_t> Apk::read(const std::string& name) const {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat(zip_, name.c_str(), ZIP_FL_ENC_RAW, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE)) {
        throw Error(path_ + ": no " + name);
    }
    zip_file_t* file = zip_fopen(zip_, name.c_str(), ZIP_FL_ENC_RAW);
    if (file == nullptr) {
        throw Error(path_ + ": can't open " + name);
    }
    std::vector<uint8_t> bytes(stat.size);
    zip_int64_t got = zip_fread(file, bytes.data(), bytes.size());
    zip_fclose(file);
    if (got < 0 || zip_uint64_t(got) != stat.size) {
        throw Error(path_ + ": short read of " + name);
    }
    return bytes;
}

std::vector<std::string> Apk::abis() const {
    std::vector<std::string> abis;
    for (const std::string& name : names()) {
        if (!name.starts_with("lib/") || !name.ends_with(".so")) {
            continue;
        }
        size_t slash = name.find('/', 4);
        if (slash == std::string::npos || name.find('/', slash + 1) != std::string::npos) {
            continue;
        }
        std::string abi = name.substr(4, slash - 4);
        if (std::find(abis.begin(), abis.end(), abi) == abis.end()) {
            abis.push_back(abi);
        }
    }
    std::sort(abis.begin(), abis.end());
    return abis;
}

std::vector<std::string> Apk::libraries(const std::string& abi) const {
    std::vector<std::string> libraries;
    std::string prefix = "lib/" + abi + "/";
    for (const std::string& name : names()) {
        if (name.starts_with(prefix) && name.ends_with(".so") && name.find('/', prefix.size()) == std::string::npos) {
            libraries.push_back(name);
        }
    }
    std::sort(libraries.begin(), libraries.end());
    return libraries;
}

bool looks_like_zip(const std::vector<uint8_t>& head) {
    return head.size() >= 4 && std::memcmp(head.data(), "PK\3\4", 4) == 0;
}

}  // namespace tsugiki::apk
