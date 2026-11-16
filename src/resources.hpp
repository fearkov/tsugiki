// android's binary resources, just enough to point qt's per-abi lib lists at x86_64
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsugiki::resources {

// resources.arsc with every "arm64-v8a;..." string turned into "x86_64;...",
// that's how qt's libs.xml lists libs per abi; none if it has no such string
std::optional<std::vector<uint8_t>> retarget_qt_libs(const std::vector<uint8_t>& arsc);

// android:value of a <meta-data android:name="name"> in a binary AndroidManifest.xml
std::optional<std::string> manifest_meta(const std::vector<uint8_t>& manifest, const std::string& name);

}  // namespace tsugiki::resources
