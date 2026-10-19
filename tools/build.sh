#!/usr/bin/env bash
# Builds the Android APK from your own copy of the game.
#
#   tools/build.sh picker  --game <CLAW-1.4.5.4-Complete-Pack.zip | extracted game folder>
#   tools/build.sh bundled --game <CLAW-1.4.5.4-Complete-Pack.zip | extracted game folder>
#
#   picker   Claw-folder-picker.apk: no game files inside; the app imports them from a
#            folder on the device (optionally compressing them there).
#   bundled  Claw-with-game-files.apk: your game files packed losslessly into the APK.
#            For your own use only: it contains the game's files.
#
# Either way your game files are needed: the app's code is produced by recompiling the
# game's CLAW.exe, and the community scripts decide what gets recompiled.
#
# Options:
#   --out DIR          where the APK goes (default: out/)
#   --abis "LIST"      Android ABIs (default: arm64-v8a; add x86_64 for emulators)
#   --skip-hash-check  accept a game pack that is not the exact supported release
#
# Requirements (all included in the Docker image, see Dockerfile and tools/build.ps1):
# bash, git, curl, 7z, python3 (venv), cmake, ninja, a C compiler, JDK 17+,
# ANDROID_HOME (platform 36, build-tools 34/35) and ANDROID_NDK_HOME (NDK r27c, 27.2.12479018).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# the supported release: CLAW v1.4.5.4 "CrazyHook" + level pack + cutscenes
PACK_SHA256=f74f908527e5c11f04682d6c944c76dd86d6926d5465e76f7093c7da39180eb7
EXE_SHA256=33cf6e2663823f073d769c3d05a1ad5e13e3e4993ad2cb6822617737395004c6

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'build: %s\n' "$*" >&2; exit 1; }
usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

MODE=${1:-}
[ "$MODE" = picker ] || [ "$MODE" = bundled ] || usage 1
shift
GAME="" OUT="$ROOT/out" ABIS="arm64-v8a" CHECK=1
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME=${2:?}; shift 2 ;;
        --out) OUT=${2:?}; shift 2 ;;
        --abis) ABIS=${2:?}; shift 2 ;;
        --skip-hash-check) CHECK=0; shift ;;
        -h|--help) usage 0 ;;
        *) die "unknown option: $1" ;;
    esac
done
[ -n "$GAME" ] || die "--game <pack zip or game folder> is required"
[ -e "$GAME" ] || die "not found: $GAME"
GAME="$(realpath "$GAME")"   # resolve before any cd
mkdir -p "$OUT"
OUT="$(realpath "$OUT")"

# ---- toolchain checks ------------------------------------------------------------------
for t in git curl 7z python3 cmake ninja cc java; do
    command -v $t >/dev/null || die "missing tool: $t (or use the Docker image: see README)"
done
: "${ANDROID_HOME:=${ANDROID_SDK_ROOT:-}}"
[ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME" ] || die "set ANDROID_HOME to the Android SDK"
export ANDROID_HOME
if [ -z "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_HOME/ndk/27.2.12479018" ]; then
    ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.2.12479018"
fi
[ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ] || die "set ANDROID_NDK_HOME to NDK r27c (27.2.12479018)"
export ANDROID_NDK_HOME

# ---- the game files ----------------------------------------------------------------------
say "Checking the game files"
if [ -f "$GAME" ]; then
    sha=$(sha256sum "$GAME" | cut -c1-64)
    if [ "$sha" != "$PACK_SHA256" ]; then
        [ $CHECK = 0 ] || die "this is not the supported pack (SHA-256 $sha); see README, or pass --skip-hash-check"
        echo "warning: unsupported pack, continuing (--skip-hash-check)"
    fi
    SRC="$ROOT/build/game"
    if [ "$(cat "$SRC/.pack-sha256" 2>/dev/null)" != "$sha" ]; then
        echo "extracting..."
        [ -d "$SRC" ] && chmod -R u+w "$SRC"
        rm -rf "$SRC" && mkdir -p "$SRC"
        7z x -y -o"$SRC" "$GAME" >/dev/null || die "could not extract $GAME"
        chmod -R u+w "$SRC"   # the zip marks its folders read-only
        echo "$sha" > "$SRC/.pack-sha256"
    fi
    EXE="$(find "$SRC" -maxdepth 3 -iname CLAW.exe -print -quit)"
else
    EXE="$(find "$GAME" -maxdepth 2 -iname CLAW.exe -print -quit)"
fi
[ -n "$EXE" ] || die "CLAW.exe not found in $GAME"
GAME_DIR="$(dirname "$EXE")"
exe_sha=$(sha256sum "$EXE" | cut -c1-64)
if [ "$exe_sha" != "$EXE_SHA256" ]; then
    [ $CHECK = 0 ] || die "CLAW.exe is not the supported version (SHA-256 $exe_sha); pass --skip-hash-check to try anyway"
    echo "warning: unsupported CLAW.exe; the recompiled code is built for it anyway"
fi
for f in CrazyHook.lua Assets; do
    [ -e "$GAME_DIR/$f" ] || die "$f missing from $GAME_DIR: use the complete CrazyHook pack"
done
echo "game folder: $GAME_DIR"

# ---- dependencies ----------------------------------------------------------------------------
say "Fetching third-party sources"
tools/fetch_deps.sh

say "Python environment"
VENV="$ROOT/build/venv"
if [ ! -x "$VENV/bin/python" ] || ! "$VENV/bin/python" -c "import capstone, pefile" 2>/dev/null; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q --disable-pip-version-check capstone==5.0.9 pefile==2024.8.26
fi
mkdir -p tools && ln -sfn "$VENV" tools/venv   # the build scripts look for tools/venv

say "Building LuaJIT and FFmpeg"
targets=""
for abi in $ABIS; do
    case $abi in arm64-v8a) targets="$targets android" ;; x86_64) targets="$targets android-x86_64" ;; *) die "unsupported ABI $abi" ;; esac
done
for t in $targets; do
    [ -f "build-luajit-$t/src/libluajit.a" ] || tools/build_luajit.sh "$t" >/dev/null
    [ -f "build-ffmpeg-$t/lib/libavcodec.a" ] || tools/build_ffmpeg.sh "$t" >/dev/null
done
if [ "$MODE" = bundled ]; then
    [ -f build-ffmpeg-host/lib/libavcodec.a ] || tools/build_ffmpeg.sh host >/dev/null
fi

# ---- recompile the game ------------------------------------------------------------------------
say "Recompiling CLAW.exe"
if [ "$(cat gen/.exe-sha256 2>/dev/null)" != "$exe_sha" ]; then
    rm -rf gen
    "$VENV/bin/python" recomp/codegen.py "$EXE" gen --config games/claw/game.ini | tail -1
    echo "$exe_sha" > gen/.exe-sha256
else
    echo "up to date"
fi

say "Building the native code"
( cd android && GAME=claw ABIS="$ABIS" ./build_native.sh >/dev/null )

PACK_ARG=()
if [ "$MODE" = bundled ]; then
    say "Packing the game files (lossless; every file verified)"
    cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME=claw >/dev/null
    cmake --build build/host --target devicepack >/dev/null
    # the packer writes game.pak inside the folder it packs: give it a symlink copy
    [ -d build/packsrc ] && find build/packsrc -type d -exec chmod u+w {} +   # a leftover from a failed run
    rm -rf build/packsrc && mkdir -p build/pack
    cp -rs "$GAME_DIR" build/packsrc
    find build/packsrc -type d -exec chmod u+w {} +   # the copy's own folders only, never the game files
    build/host/devicepack build/packsrc 2>&1 | tr '\r' '\n' | grep -E "^(ok|packer)|fail|cannot" || true
    [ -f build/packsrc/game.pak ] || die "packing failed"
    mv build/packsrc/game.pak build/pack/game.pak
    rm -rf build/packsrc
    PACK_ARG=(-Ppack="$ROOT/build/pack/game.pak")
fi

say "Building the APK"
( cd android && rm -rf app/build/gameassets && ./gradlew --no-daemon -q assembleRelease \
    -Pabis="$(echo $ABIS | tr ' ' ,)" -Pgame=claw "${PACK_ARG[@]}" )
NAME=Claw-folder-picker.apk
[ "$MODE" = bundled ] && NAME=Claw-with-game-files.apk
cp android/app/build/outputs/apk/release/app-release.apk "$OUT/$NAME"

say "Done"
ls -l "$OUT/$NAME"
echo "SHA-256: $(sha256sum "$OUT/$NAME" | cut -c1-64)"
[ "$MODE" = bundled ] && echo "This APK contains the game's files: keep it for your own use."
true
