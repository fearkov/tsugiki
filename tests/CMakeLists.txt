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

# integer kernels: arm64 translated vs the same c built natively
set(KERNELS_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/data/kernels.c)
set(KERNELS_LIBRARY ${SAMPLE_DIR}/libkernels.so)
set(KERNELS_TRANSLATED ${CMAKE_CURRENT_BINARY_DIR}/kernels_translated.cpp)
add_custom_command(
    OUTPUT ${KERNELS_LIBRARY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SAMPLE_DIR}
    COMMAND ${CLANG} ${SAMPLE_FLAGS} -mgeneral-regs-only -mno-outline-atomics -o ${KERNELS_LIBRARY} ${KERNELS_SOURCE}
    DEPENDS ${KERNELS_SOURCE}
    VERBATIM)
add_custom_command(
    OUTPUT ${KERNELS_TRANSLATED}
    COMMAND tsugiki translate ${KERNELS_LIBRARY} kernels ${KERNELS_TRANSLATED}
    DEPENDS tsugiki ${KERNELS_LIBRARY}
    VERBATIM)
add_library(kernels_native OBJECT data/kernels.c)
target_compile_definitions(kernels_native PRIVATE NATIVE)
add_library(kernels_translated OBJECT ${KERNELS_TRANSLATED})
target_include_directories(kernels_translated PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
target_compile_options(kernels_translated PRIVATE -O2 -mcx16 -w)

# fp and vectorized loops: arm64 -O3 translated vs native
set(VECTORS_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/data/vectors.c)
set(VECTORS_LIBRARY ${SAMPLE_DIR}/libvectors.so)
set(VECTORS_TRANSLATED ${CMAKE_CURRENT_BINARY_DIR}/vectors_translated.cpp)
set(FP_FLAGS -O3 -ffp-contract=off -fno-math-errno)
add_custom_command(
    OUTPUT ${VECTORS_LIBRARY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SAMPLE_DIR}
    COMMAND ${CLANG} ${SAMPLE_FLAGS} ${FP_FLAGS} -mno-outline-atomics -o ${VECTORS_LIBRARY} ${VECTORS_SOURCE}
    DEPENDS ${VECTORS_SOURCE}
    VERBATIM)
add_custom_command(
    OUTPUT ${VECTORS_TRANSLATED}
    COMMAND tsugiki translate ${VECTORS_LIBRARY} vectors ${VECTORS_TRANSLATED}
    DEPENDS tsugiki ${VECTORS_LIBRARY}
    VERBATIM)
add_library(vectors_native OBJECT data/vectors.c)
target_compile_definitions(vectors_native PRIVATE NATIVE)
target_compile_options(vectors_native PRIVATE ${FP_FLAGS})
add_library(vectors_translated OBJECT ${VECTORS_TRANSLATED})
target_include_directories(vectors_translated PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
target_compile_options(vectors_translated PRIVATE -O2 -mcx16 -w)

add_library(tsugiki_host_runtime STATIC ${PROJECT_SOURCE_DIR}/runtime/host.cpp ${PROJECT_SOURCE_DIR}/runtime/simd.cpp)
target_include_directories(tsugiki_host_runtime PUBLIC ${PROJECT_SOURCE_DIR}/runtime)

add_executable(tsugiki_tests main.cpp a64_test.cpp bytes_test.cpp elf_test.cpp translate_test.cpp vectors_test.cpp loader.cpp
    $<TARGET_OBJECTS:kernels_native> $<TARGET_OBJECTS:kernels_translated>
    $<TARGET_OBJECTS:vectors_native> $<TARGET_OBJECTS:vectors_translated>)
target_link_libraries(tsugiki_tests PRIVATE tsugiki_core tsugiki_host_runtime)
target_compile_options(tsugiki_tests PRIVATE -Wall -Wextra)
target_compile_definitions(tsugiki_tests PRIVATE
    SAMPLE_DIR="${SAMPLE_DIR}"
    CLANG="${CLANG}"
    LLVM_READELF="${LLVM_READELF}"
    LLVM_OBJDUMP="${LLVM_OBJDUMP}")
add_dependencies(tsugiki_tests samples)
add_test(NAME tsugiki_tests COMMAND tsugiki_tests)
