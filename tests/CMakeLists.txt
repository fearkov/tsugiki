# arm64 samples built right here with clang + lld, no ndk needed
# same code, every relocation packing android's linker reads
find_program(CLANG NAMES clang)
find_program(LLVM_READELF NAMES llvm-readelf)
find_program(LLVM_OBJDUMP NAMES llvm-objdump)
if(NOT CLANG)
    message(WARNING "no clang, skipping the tests")
    return()
endif()

set(SAMPLE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/data/sample.c)
set(SAMPLE_DIR ${CMAKE_CURRENT_BINARY_DIR}/samples)
set(SAMPLE_FLAGS --target=aarch64-linux-android29 -ffreestanding -fPIC -shared -nostdlib -fuse-ld=lld -O2
    -funwind-tables -fasynchronous-unwind-tables -Wl,--eh-frame-hdr)

set(flags_rela -Wl,-z,nopack-relative-relocs -Wl,--hash-style=both)
set(flags_relr -Wl,-z,pack-relative-relocs -Wl,--hash-style=both)
set(flags_android -Wl,--pack-dyn-relocs=android -Wl,--hash-style=both)
set(flags_android_relr -Wl,--pack-dyn-relocs=android+relr -Wl,--hash-style=both)
set(flags_android_tags -Wl,--pack-dyn-relocs=relr -Wl,--use-android-relr-tags -Wl,--hash-style=both)
set(flags_gnu_hash -Wl,--hash-style=gnu)
set(flags_sysv_hash -Wl,--hash-style=sysv)
set(VARIANTS rela relr android android_relr android_tags gnu_hash sysv_hash)

set(SAMPLES)
foreach(variant IN LISTS VARIANTS)
    set(out ${SAMPLE_DIR}/libsample_${variant}.so)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SAMPLE_DIR}
        COMMAND ${CLANG} ${SAMPLE_FLAGS} ${flags_${variant}} -o ${out} ${SAMPLE_SOURCE}
        DEPENDS ${SAMPLE_SOURCE}
        VERBATIM)
    list(APPEND SAMPLES ${out})
endforeach()
add_custom_target(samples DEPENDS ${SAMPLES})

add_executable(tsugiki_tests main.cpp a64_test.cpp bytes_test.cpp elf_test.cpp)
target_link_libraries(tsugiki_tests PRIVATE tsugiki_core)
target_compile_options(tsugiki_tests PRIVATE -Wall -Wextra)
target_compile_definitions(tsugiki_tests PRIVATE
    SAMPLE_DIR="${SAMPLE_DIR}"
    CLANG="${CLANG}"
    LLVM_READELF="${LLVM_READELF}"
    LLVM_OBJDUMP="${LLVM_OBJDUMP}")
add_dependencies(tsugiki_tests samples)
add_test(NAME tsugiki_tests COMMAND tsugiki_tests)
