// building translated libs for android x86_64 with the ndk
#pragma once

#include <string>
#include <vector>

#include "elf.hpp"
#include "lift/lift.hpp"

namespace tsugiki::android {

// a c identifier from a soname, libfoo-bar.so -> foo_bar
std::string library_id(const std::string& soname);

// the module descriptor and its constructor, or main for executables
std::string module_cpp(const std::string& id, const std::string& soname, bool executable);

// the embedded image and a wrapper per exported function
std::string glue_asm(const elf::Library& library, const std::string& id, const std::string& image_path, bool executable);

// has PT_INTERP, android ships some of those as lib*.so
bool is_executable(const elf::Library& library);

// the ndk, from ANDROID_NDK_HOME or ~/Android/Sdk/ndk/<newest>
std::string ndk();

// clang++ for x86_64 android
std::string compiler();

// libtsugiki.so into dir
void build_runtime(const std::string& dir);

// TSUGIKI_JOBS or the cores, at most 8
int default_jobs();

// translated libs into dir, libtsugiki.so already there; all their parts compile together
void build_libraries(const std::vector<std::string>& paths, const std::string& dir, int jobs);

// an arm64 apk into an x86_64 one: libs translated, aligned, signed with our own key
// force rebuilds an apk that already has x86_64 libs from its arm64 ones
void build_apk(const std::string& in, const std::string& out, bool force, int jobs);

// paths of libs, deps first
std::vector<std::string> build_order(const std::vector<std::string>& paths);

}  // namespace tsugiki::android
