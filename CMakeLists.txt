cmake_minimum_required(VERSION 3.24)
project(tsugiki LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)

add_library(tsugiki_core STATIC
    src/a64/decoder.cpp
    src/android.cpp
    src/a64/simd.cpp
    src/apk.cpp
    src/eh_frame.cpp
    src/elf.cpp
    src/lift/lift.cpp
    src/lift/simd.cpp
    src/lift/tables.cpp
)
target_include_directories(tsugiki_core PUBLIC src)
target_link_libraries(tsugiki_core PUBLIC PkgConfig::LIBZIP)
target_compile_options(tsugiki_core PRIVATE -Wall -Wextra -Wpedantic)
# the runtime gets built from here for android
target_compile_definitions(tsugiki_core PRIVATE TSUGIKI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(tsugiki src/main.cpp)
target_link_libraries(tsugiki PRIVATE tsugiki_core)
target_compile_options(tsugiki PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
