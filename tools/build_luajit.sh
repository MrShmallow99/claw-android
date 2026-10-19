#!/bin/bash
# usage: tools/build_luajit.sh host|android|android-x86_64  -- builds build-luajit-<target>/src/libluajit.a
# LuaJIT 2.1 patched for the guest-memory FFI (tools/luajit_guest32.py), interpreter only.
set -e
cd "$(dirname "$0")/.."
T=${1:-host}
ROOT=$(pwd)
PY=tools/venv/bin/python; [ -x "$PY" ] || PY=python3   # the patch script needs only the standard library
"$PY" tools/luajit_guest32.py third_party/luajit
rm -rf build-luajit-$T && cp -r third_party/luajit build-luajit-$T
cd build-luajit-$T/src
X="-DLUAJIT_DISABLE_JIT -DLUAJIT_GUEST32"
TC="-include $ROOT/runtime/lua/port_lua_io.h"
if [ "${T%%-*}" = android ]; then
  NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK:-$ROOT/third_party/android-ndk-r27c}}
  B=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
  CC="$B/aarch64-linux-android24-clang"
  [ "$T" = android-x86_64 ] && CC="$B/x86_64-linux-android24-clang"
  make -j$(nproc) libluajit.a BUILDMODE=static HOST_CC="gcc" CROSS="$B/llvm-" STATIC_CC="$CC" DYNAMIC_CC="$CC -fPIC" \
    TARGET_LD="$CC" TARGET_AR="$B/llvm-ar rcus" TARGET_STRIP="$B/llvm-strip" TARGET_SYS=Linux \
    XCFLAGS="$X" TARGET_CFLAGS="$TC" CFLAGS="-O2 -fPIC"
else
  make -j$(nproc) libluajit.a BUILDMODE=static XCFLAGS="$X" TARGET_CFLAGS="$TC" CFLAGS="-O2 -fPIC"
fi
ls -la libluajit.a
