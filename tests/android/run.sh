#!/bin/sh
# builds the device check and runs it on whatever adb sees
# usage: tests/android/run.sh <tsugiki build dir> [kernels|vectors|bridge]
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${1:-$root/build}" && pwd)
ndk=${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Android/Sdk/ndk/* | sort | tail -1)}
bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
out=$build/android
mkdir -p "$out"

# arm64 bridge lib, against the ndk's libc
"$bin/clang" --target=aarch64-linux-android29 -O2 -fPIC -shared -o "$out/arm64-libbridge.so" "$root/tests/data/bridge.c" -lOpenSLES
# c++ with its libc++ and unwinder inside
"$bin/clang++" --target=aarch64-linux-android29 -O2 -fPIC -shared -static-libstdc++ -o "$out/arm64-libcxx.so" "$root/tests/data/cxx.cpp"
cp "$build/tests/samples/libkernels.so" "$out/arm64-libkernels.so"
cp "$build/tests/samples/libvectors.so" "$out/arm64-libvectors.so"
for lib in kernels vectors bridge cxx; do
    # the soname comes from the file name when there's none
    mkdir -p "$out/in" && cp "$out/arm64-lib$lib.so" "$out/in/lib$lib.so"
done
"$build/tsugiki" build "$out/in/libkernels.so" "$out/in/libvectors.so" "$out/in/libbridge.so" "$out/in/libcxx.so" -o "$out/x86_64"

# the same c built natively to compare
"$bin/clang" --target=x86_64-linux-android29 -O2 -DNATIVE -c -o "$out/kernels.o" "$root/tests/data/kernels.c"
"$bin/clang" --target=x86_64-linux-android29 -O3 -ffp-contract=off -fno-math-errno -DNATIVE -c -o "$out/vectors.o" "$root/tests/data/vectors.c"
"$bin/clang++" --target=x86_64-linux-android29 -O2 -DNATIVE -c -o "$out/cxx.o" "$root/tests/data/cxx.cpp"
"$bin/clang++" --target=x86_64-linux-android29 -std=c++20 -O2 -static-libstdc++ -o "$out/check" "$root/tests/android/check.cpp" "$out/kernels.o" "$out/vectors.o" "$out/cxx.o" -ldl

command -v adb >/dev/null || PATH=${ANDROID_HOME:-$HOME/Android/Sdk}/platform-tools:$PATH
dir=/data/local/tmp/tsugiki
adb shell mkdir -p $dir
adb push "$out/check" "$out/x86_64/libtsugiki.so" "$out/x86_64/libkernels.so" "$out/x86_64/libvectors.so" "$out/x86_64/libbridge.so" "$out/x86_64/libcxx.so" $dir/ >/dev/null
adb shell "cd $dir && LD_LIBRARY_PATH=$dir ./check $2"
