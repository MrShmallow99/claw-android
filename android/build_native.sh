#!/bin/bash
# Builds libSDL2.so and libmain.so (the recompiled game) for Android with the NDK,
# placing them in android/native-libs/<abi>/ where Gradle picks them up.
# GAME=<name> selects games/<name>/game.ini (default claw); the generated C
# must already be in gen/ (recomp/codegen.py <exe> gen --config games/<name>/game.ini).
set -e
cd "$(dirname "$0")/.."
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK:-$PWD/third_party/android-ndk-r27c}}
ABIS=${ABIS:-"arm64-v8a"}
GAME=${GAME:-claw}
for ABI in $ABIS; do
    B=build-android/$ABI
    cmake -S . -B $B -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-24 \
        -DCMAKE_BUILD_TYPE=Release -DGAME="$GAME"
    cmake --build $B -j"$(nproc)"
    mkdir -p android/native-libs/$ABI
    cp $B/sdl2/libSDL2.so $B/libmain.so android/native-libs/$ABI/
    $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded android/native-libs/$ABI/*.so
done
for ABI in $(ls android/native-libs); do
    case " $ABIS " in *" $ABI "*) ;; *) rm -rf android/native-libs/$ABI ;; esac
done
ls -la android/native-libs/*/
