#include "android.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <thread>

#include <zip.h>

#include "apk.hpp"
#include "bytes.hpp"

namespace fs = std::filesystem;

namespace tsugiki::android {

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file) {
        throw Error(path.string() + ": can't write");
    }
}

void run(const std::string& command) {
    if (std::system(command.c_str()) != 0) {
        throw Error("failed: " + command);
    }
}

// stderr kept aside, shown only if it fails, java tools are chatty
void run_quiet(const std::string& command, const fs::path& log) {
    if (std::system((command + " 2>'" + log.string() + "'").c_str()) != 0) {
        throw Error("failed: " + command + "\n" + read_text(log));
    }
}

std::string quote(const std::string& s) {
    return "'" + s + "'";
}

// android system libs a guest may need, as -l flags
const std::set<std::string> system_libs = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so",
                                           "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libOpenMAXAL.so", "libjnigraphics.so", "libz.so",
                                           "libvulkan.so", "libaaudio.so", "libmediandk.so", "libcamera2ndk.so", "libnativewindow.so", "libsync.so",
                                           "libneuralnetworks.so", "libbinder_ndk.so", "libicu.so", "libamidi.so", "libstdc++.so"};

elf::Library read_library(const std::string& path) {
    std::string bytes = read_text(path);
    return elf::Library(std::vector<uint8_t>(bytes.begin(), bytes.end()), path);
}

std::string soname_of(const elf::Library& library, const std::string& path) {
    return library.soname.empty() ? fs::path(path).filename().string() : library.soname;
}

// runtime sources newer than the built one
bool stale(const fs::path& built, const std::vector<std::string>& sources) {
    if (!fs::exists(built)) {
        return true;
    }
    auto when = fs::last_write_time(built);
    for (const std::string& source : sources) {
        if (fs::last_write_time(source) > when) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string library_id(const std::string& soname) {
    std::string name = soname;
    if (name.starts_with("lib")) name = name.substr(3);
    if (name.ends_with(".so")) name = name.substr(0, name.size() - 3);
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return name;
}

std::string module_cpp(const std::string& id, const std::string& soname, bool executable) {
    std::string m = "tsugiki_module_" + id;
    std::string size = "    " + m + ".image_size = uint64_t(tsugiki_image_" + id + "_end - tsugiki_image_" + id + ");\n" +
                       "    " + m + ".space_size = uint64_t(tsugiki_space_" + id + "_end - tsugiki_space_" + id + ");\n";
    std::string out = "extern \"C\" const uint8_t tsugiki_image_" + id + "[];\n" +
                      "extern \"C\" const uint8_t tsugiki_image_" + id + "_end[];\n" +
                      "extern \"C\" uint8_t tsugiki_space_" + id + "[];\n" +
                      "extern \"C\" uint8_t tsugiki_space_" + id + "_end[];\n" +
                      "extern \"C\" __attribute__((visibility(\"hidden\"))) tsugiki_module " + m + " = {\"" + soname + "\", tsugiki_entries_" + id +
                      ", tsugiki_entry_count_" + id + ", &tsugiki_base_" + id + ", tsugiki_image_" + id + ", 0, nullptr, tsugiki_space_" + id + ", 0};\n";
    if (executable) {
        return out + "int main(int argc, char** argv, char** envp) {\n" + size + "    return tsugiki_main(&" + m + ", argc, argv, envp);\n}\n";
    }
    return out + "__attribute__((constructor)) static void tsugiki_init() {\n" + size + "    tsugiki_load(&" + m + ");\n}\n";
}

std::string glue_asm(const elf::Library& library, const std::string& id, const std::string& image_path, bool executable) {
    std::string out;
    out += "    .section .rodata.tsugiki_image, \"a\"\n    .balign 16\n";
    out += "    .globl tsugiki_image_" + id + "\n    .hidden tsugiki_image_" + id + "\ntsugiki_image_" + id + ":\n";
    out += "    .incbin \"" + image_path + "\"\n";
    out += "    .globl tsugiki_image_" + id + "_end\n    .hidden tsugiki_image_" + id + "_end\ntsugiki_image_" + id + "_end:\n\n";
    // room for the image at run time, 64k aligned by the runtime
    uint64_t extent = 0;
    for (const elf::Segment& s : library.segments()) {
        if (s.type == elf::segment::load) {
            extent = std::max(extent, s.vaddr + s.memsz);
        }
    }
    uint64_t space = ((extent + 0xffff) & ~uint64_t(0xffff)) + 0x10000;
    out += "    .section .bss.tsugiki_space, \"aw\", @nobits\n    .balign 4096\n";
    out += "    .globl tsugiki_space_" + id + "\n    .hidden tsugiki_space_" + id + "\ntsugiki_space_" + id + ":\n";
    out += "    .zero " + std::to_string(space) + "\n";
    out += "    .globl tsugiki_space_" + id + "_end\n    .hidden tsugiki_space_" + id + "_end\ntsugiki_space_" + id + "_end:\n\n    .text\n";
    std::set<std::string> done;
    for (const elf::Symbol& s : library.dynamic_symbols) {
        if (executable || !s.defined() || !s.code() || s.bind == elf::symbol::local || s.visibility == 2 || s.name.empty() || done.contains(s.name)) {
            continue;
        }
        done.insert(s.name);
        const char* entry = "tsugiki_export_entry";
        if (s.name == "JNI_OnLoad" || s.name == "JNI_OnUnload") {
            entry = "tsugiki_jni_onload_entry";
        } else if (s.name.starts_with("Java_")) {
            entry = "tsugiki_jni_entry";
        }
        char vaddr[32];
        std::snprintf(vaddr, sizeof vaddr, "0x%llx", static_cast<unsigned long long>(s.value));
        out += (s.bind == elf::symbol::weak ? "    .weak " : "    .globl ") + s.name + "\n";
        out += "    .type " + s.name + ", @function\n" + s.name + ":\n";
        out += std::string("    movabs $") + vaddr + ", %r10\n";
        out += "    lea tsugiki_module_" + id + "(%rip), %r11\n";
        out += std::string("    jmp ") + entry + "@PLT\n";
        out += "    .size " + s.name + ", . - " + s.name + "\n";
    }
    out += "\n    .section .note.GNU-stack, \"\", @progbits\n";
    return out;
}

std::string ndk() {
    for (const char* var : {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"}) {
        if (const char* value = std::getenv(var); value && fs::exists(value)) {
            return value;
        }
    }
    const char* home = std::getenv("HOME");
    fs::path dir = fs::path(home ? home : "") / "Android/Sdk/ndk";
    std::vector<fs::path> versions;
    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            versions.push_back(entry.path());
        }
    }
    if (versions.empty()) {
        throw Error("no android ndk, set ANDROID_NDK_HOME");
    }
    std::sort(versions.begin(), versions.end());
    return versions.back().string();
}

std::string compiler() {
    return ndk() + "/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ --target=x86_64-linux-android29";
}

void build_runtime(const std::string& dir) {
    fs::create_directories(dir);
    std::string source = TSUGIKI_SOURCE_DIR;
    std::vector<std::string> files;
    for (const char* file : {"runtime/android/runtime.cpp", "runtime/android/libc.cpp", "runtime/android/jni.cpp", "runtime/android/entries.cpp",
                             "runtime/android/system.cpp", "runtime/android/entry.S", "runtime/simd.cpp", "src/elf.cpp", "src/eh_frame.cpp",
                             "runtime/android/runtime.hpp", "runtime/tsugiki.h", "runtime/tsugiki_simd.h", "src/elf.hpp", "src/eh_frame.hpp"}) {
        files.push_back(source + "/" + file);
    }
    if (!stale(fs::path(dir) / "libtsugiki.so", files)) {
        return;
    }
    std::string sources;
    for (const std::string& file : files) {
        if (!file.ends_with(".hpp") && !file.ends_with(".h")) {
            sources += " " + quote(file);
        }
    }
    // hidden and bound to itself: a guest lib re-exporting its static libc++ must never catch our own calls
    run(compiler() + " -std=c++20 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -I" + quote(source + "/runtime") + " -I" +
        quote(source + "/src") + sources + " -llog -ldl -static-libstdc++ -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -Wl,-soname,libtsugiki.so -o " +
        quote(dir + "/libtsugiki.so"));
}

bool is_executable(const elf::Library& library) {
    return library.segment(elf::segment::interp).has_value();
}

int default_jobs() {
    if (const char* jobs = std::getenv("TSUGIKI_JOBS")) {
        return std::max(1, std::atoi(jobs));
    }
    // about 200 MB a part, keep it modest
    return int(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
}

namespace {

// what's left to do for one lib once its sources are written
struct Prepared {
    std::vector<std::string> compiles;
    std::string link;
};

// the c++ parts and glue of one lib, nothing if it's up to date
std::optional<Prepared> prepare(const std::string& path, const std::string& dir, const std::set<std::string>& translated) {
    fs::path work = fs::path(dir) / "work";
    fs::create_directories(work);
    elf::Library library = read_library(path);
    std::string soname = soname_of(library, path);
    // like make: nothing newer than what we built last time, the translator and its headers included
    std::string source = TSUGIKI_SOURCE_DIR;
    if (!stale(fs::path(dir) / soname, {path, fs::read_symlink("/proc/self/exe").string(), source + "/runtime/tsugiki.h", source + "/runtime/tsugiki_simd.h"})) {
        return std::nullopt;
    }
    std::string id = library_id(soname);
    bool executable = is_executable(library);
    fs::path base = work / fs::path(soname).stem();
    std::string image = base.string() + ".arm64";
    fs::copy_file(path, image, fs::copy_options::overwrite_existing);

    auto program = lift::discover(library);
    // about 30k instructions a file, each a few seconds
    auto parts = lift::emit_parts(program, id, 30000);
    parts[0] += module_cpp(id, soname, executable);
    write_text(base.string() + "_glue.S", glue_asm(library, id, fs::absolute(image).string(), executable));

    Prepared p;
    std::string runtime = source + "/runtime";
    std::string inputs;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string file = base.string() + "_" + std::to_string(i) + ".cpp";
        std::string object = base.string() + "_" + std::to_string(i) + ".o";
        write_text(file, parts[i]);
        p.compiles.push_back(compiler() + " -std=c++20 -O2 -w -mcx16 -fPIC -I" + quote(runtime) + " -c " + quote(file) + " -o " + quote(object));
        inputs += " " + quote(object);
    }

    std::string links;
    for (const std::string& needed : library.needed) {
        if (needed == "libc.so" || needed == "libstdc++.so") {
            continue;
        }
        if (system_libs.contains(needed)) {
            links += " -l" + needed.substr(3, needed.size() - 6);
        } else if (translated.contains(needed) || fs::exists(fs::path(dir) / needed)) {
            // another translated lib, linked before this one
            links += " " + quote((fs::path(dir) / needed).string());
        }
    }
    std::string common = compiler() + inputs + " " + quote(base.string() + "_glue.S");
    std::string out = quote((fs::path(dir) / soname).string());
    if (executable) {
        // its own process: libtsugiki.so sits next to it
        p.link = common + " -fPIE -L" + quote(dir) + " -ltsugiki" + links + " -nostdlib++ -Wl,-rpath,'$ORIGIN' -o " + out;
    } else {
        p.link = common + " -fPIC -shared -L" + quote(dir) + " -ltsugiki" + links + " -nostdlib++ -Wl,--no-undefined -Wl,-soname," + soname + " -o " + out;
    }
    return p;
}

void run_all(const std::vector<std::string>& commands, int jobs) {
    std::atomic<size_t> next{0};
    std::mutex failure_lock;
    std::string failure;
    std::vector<std::thread> workers;
    for (int w = 0; w < std::max(1, jobs) && size_t(w) < commands.size(); ++w) {
        workers.emplace_back([&] {
            for (size_t i; (i = next++) < commands.size();) {
                try {
                    run(commands[i]);
                } catch (const std::exception& e) {
                    std::lock_guard guard(failure_lock);
                    failure = e.what();
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    if (!failure.empty()) {
        throw Error(failure);
    }
}

}  // namespace

void build_libraries(const std::vector<std::string>& paths, const std::string& dir, int jobs) {
    // every part of every lib in one pool, then the links in dependency order
    std::vector<std::string> compiles, links;
    std::set<std::string> translated;
    for (const std::string& path : paths) {
        translated.insert(soname_of(read_library(path), path));
    }
    for (const std::string& path : build_order(paths)) {
        auto p = prepare(path, dir, translated);
        if (!p) {
            continue;
        }
        std::printf("%s\n", fs::path(path).filename().c_str());
        std::fflush(stdout);
        compiles.insert(compiles.end(), p->compiles.begin(), p->compiles.end());
        links.push_back(p->link);
    }
    run_all(compiles, jobs);
    for (const std::string& link : links) {
        run(link);
    }
}

std::vector<std::string> build_order(const std::vector<std::string>& paths) {
    std::map<std::string, std::vector<std::string>> needs;
    std::map<std::string, std::string> path_of;
    for (const std::string& path : paths) {
        elf::Library library = read_library(path);
        std::string soname = soname_of(library, path);
        needs[soname] = library.needed;
        path_of[soname] = path;
    }
    std::vector<std::string> order;
    std::set<std::string> done, visiting;
    auto visit = [&](auto&& self, const std::string& soname) -> void {
        if (done.contains(soname) || !needs.contains(soname) || visiting.contains(soname)) {
            return;
        }
        visiting.insert(soname);
        for (const std::string& dep : needs[soname]) {
            self(self, dep);
        }
        done.insert(soname);
        order.push_back(path_of[soname]);
    };
    for (const auto& [soname, _] : needs) {
        visit(visit, soname);
    }
    return order;
}

namespace {

std::string sdk() {
    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (const char* value = std::getenv(var); value && fs::exists(value)) {
            return value;
        }
    }
    const char* home = std::getenv("HOME");
    fs::path dir = fs::path(home ? home : "") / "Android/Sdk";
    if (!fs::exists(dir)) {
        throw Error("no android sdk, set ANDROID_HOME");
    }
    return dir.string();
}

std::string build_tool(const std::string& name) {
    std::vector<fs::path> versions;
    fs::path dir = fs::path(sdk()) / "build-tools";
    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (fs::exists(entry.path() / name)) {
                versions.push_back(entry.path());
            }
        }
    }
    if (versions.empty()) {
        throw Error("no " + name + " in the sdk's build-tools");
    }
    std::sort(versions.begin(), versions.end());
    return (versions.back() / name).string();
}

// our signing key, made once, the app gets a new signature anyway
std::pair<std::string, std::string> signing_key() {
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path dir = data ? fs::path(data) / "tsugiki" : fs::path(home ? home : "") / ".local/share/tsugiki";
    fs::path key = dir / "key.pk8", cert = dir / "cert.pem";
    if (!fs::exists(key) || !fs::exists(cert)) {
        fs::create_directories(dir);
        fs::path pem = dir / "key.pem";
        run("openssl req -x509 -newkey rsa:2048 -sha256 -days 10000 -nodes -subj /CN=tsugiki -keyout " + quote(pem.string()) + " -out " +
            quote(cert.string()) + " 2>/dev/null");
        run("openssl pkcs8 -topk8 -inform PEM -outform DER -nocrypt -in " + quote(pem.string()) + " -out " + quote(key.string()));
        fs::remove(pem);
    }
    return {key.string(), cert.string()};
}

// the old v1 signature, apksigner makes a new one
bool signature_file(const std::string& name) {
    if (!name.starts_with("META-INF/") || name.find('/', 9) != std::string::npos) {
        return false;
    }
    return name == "META-INF/MANIFEST.MF" || name.ends_with(".SF") || name.ends_with(".RSA") || name.ends_with(".DSA") || name.ends_with(".EC");
}

void check_zip(int error, const std::string& what) {
    if (error != 0) {
        throw Error(what);
    }
}

}  // namespace

void build_apk(const std::string& in, const std::string& out, bool force, int jobs) {
    fs::path work = fs::path(out).replace_extension(".work");
    fs::create_directories(work / "arm64");
    std::vector<std::string> paths;
    {
        apk::Apk apk(in);
        auto abis = apk.abis();
        if (std::find(abis.begin(), abis.end(), "arm64-v8a") == abis.end()) {
            throw Error(in + ": no arm64-v8a libs, nothing to translate");
        }
        if (std::find(abis.begin(), abis.end(), "x86_64") != abis.end() && !force) {
            throw Error(in + ": already has x86_64 libs, --force rebuilds them from arm64");
        }
        for (const std::string& name : apk.libraries("arm64-v8a")) {
            fs::path path = work / "arm64" / fs::path(name).filename();
            auto bytes = apk.read(name);
            std::string text(bytes.begin(), bytes.end());
            // untouched if it's the same, so its translation isn't redone
            if (!fs::exists(path) || read_text(path) != text) {
                write_text(path, text);
            }
            paths.push_back(path.string());
        }
    }
    fs::path libs = work / "x86_64";
    build_runtime(libs.string());
    build_libraries(paths, libs.string(), jobs);

    // the same apk, every lib dir swapped for ours, stored so they load in place
    fs::path unsigned_apk = work / "unsigned.apk", aligned = work / "aligned.apk";
    fs::copy_file(in, unsigned_apk, fs::copy_options::overwrite_existing);
    int error = 0;
    zip_t* z = zip_open(unsigned_apk.c_str(), 0, &error);
    if (z == nullptr) {
        throw Error(unsigned_apk.string() + ": can't open");
    }
    zip_int64_t count = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        const char* name = zip_get_name(z, zip_uint64_t(i), ZIP_FL_ENC_RAW);
        if (name && (std::string(name).starts_with("lib/") || signature_file(name))) {
            check_zip(zip_delete(z, zip_uint64_t(i)), std::string("can't drop ") + name);
        }
    }
    for (const auto& entry : fs::directory_iterator(libs)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".so") {
            continue;
        }
        zip_source_t* source = zip_source_file(z, entry.path().c_str(), 0, ZIP_LENGTH_TO_END);
        std::string name = "lib/x86_64/" + entry.path().filename().string();
        zip_int64_t index = source ? zip_file_add(z, name.c_str(), source, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0) {
            zip_source_free(source);
            throw Error("can't add " + name);
        }
        check_zip(zip_set_file_compression(z, zip_uint64_t(index), ZIP_CM_STORE, 0), "can't store " + name);
    }
    if (zip_close(z) != 0) {
        std::string message = zip_strerror(z);
        zip_discard(z);
        throw Error(unsigned_apk.string() + ": " + message);
    }
    run(quote(build_tool("zipalign")) + " -f -P 16 4 " + quote(unsigned_apk.string()) + " " + quote(aligned.string()));
    auto [key, cert] = signing_key();
    run_quiet(quote(build_tool("apksigner")) + " sign --key " + quote(key) + " --cert " + quote(cert) + " --out " + quote(out) + " " + quote(aligned.string()),
              work / "apksigner.log");
    fs::remove(fs::path(out).string() + ".idsig");
}

}  // namespace tsugiki::android
