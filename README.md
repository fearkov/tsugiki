# tsugiki

AOT recompiler that turns ARM-64-only android apps into x86-64 APKs.

Native bridges like libhoudini or libndk_translation translate arm64 code while
the app runs, and they have to be part of the system image. tsugiki translates
an app's `lib/arm64-v8a/*.so` ahead of time into x86_64 libraries instead, so the
result is a plain x86_64 APK that installs on Waydroid, the emulator or
Android-x86 as is.

## Status

Early. What works so far:

- reading APKs and their arm64 libraries
- ELF: segments, dynamic table, symbols, relocations as RELA, RELR (standard and
  Android tags) and Android's packed APS2
- function ranges from `.eh_frame`

```
tsugiki info app.apk
tsugiki info libgame.so
```

## Building

Needs CMake, a C++20 compiler and libzip. The tests also need clang, lld and
llvm-readelf, which build arm64 sample libraries on the spot, no NDK needed.

```
cmake -S . -B build
make -C build -j
./build/tests/tsugiki_tests
```

## Note

Translated APKs are the app's own code, so they're for your machine only. Never
share them.
