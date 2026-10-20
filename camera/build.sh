#!/bin/sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../../.." && pwd)
NDK=${NDK:-$HOME/Android/Sdk/ndk/23.2.8568313}
OUT=${1:-$TOP/out/target/product/wave}
CXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi19-clang++

LIBS=$HERE/build/lib
mkdir -p "$LIBS" "$HERE/prebuilt"
cp "$OUT/system/lib/libhardware.so" "$LIBS/"

"$CXX" -std=c++20 -O2 -g0 -fPIC -shared \
    -mthumb -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp \
    -fno-exceptions -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden \
    -ffunction-sections -fdata-sections \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -I"$TOP/hardware/libhardware/include" -I"$TOP/system/core/include" \
    "$HERE"/src/*.cpp \
    -static-libstdc++ -L"$LIBS" -lhardware -llog \
    -Wl,--gc-sections -Wl,--hash-style=both -Wl,--no-undefined -Wl,-z,noexecstack \
    -Wl,--exclude-libs,ALL -Wl,-soname,camera.wave.so -s \
    -o "$HERE/prebuilt/camera.wave.so"
echo "$HERE/prebuilt/camera.wave.so"
