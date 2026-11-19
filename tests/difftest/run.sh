#!/bin/sh
# lifter vs interpreter, one instruction at a time, on every word some real libs have
# usage: tests/difftest/run.sh <tsugiki build dir> <lib.so>... [-- rounds]
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "$1" && pwd)
shift
libs=""
rounds=64
while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then rounds=$2; break; fi
    libs="$libs $1"
    shift
done
out=$build/difftest
mkdir -p "$out/parts"
rm -f "$out"/parts/*.cpp
make -C "$build" -s tsugiki_core
cxx=${CXX:-clang++}
$cxx -std=c++20 -O1 -I"$root/src" "$root/tests/difftest/gen.cpp" "$build/libtsugiki_core.a" $(pkg-config --libs libzip) -o "$out/gen"
"$out/gen" "$out/parts" $libs
$cxx -std=c++20 -O1 -w -mcx16 -I"$root/runtime" -I"$root/src" "$root/tests/difftest/test.cpp" "$out"/parts/*.cpp "$root/runtime/interp.cpp" \
    "$root/runtime/host.cpp" "$root/runtime/simd.cpp" "$build/libtsugiki_core.a" $(pkg-config --libs libzip) -o "$out/test"
"$out/test" "$rounds"
