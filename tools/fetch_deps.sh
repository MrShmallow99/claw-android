#!/usr/bin/env bash
# Downloads the third-party sources the build needs into third_party/, each pinned to
# an exact version and checked against its SHA-256 (git checkouts: by commit).
# Safe to re-run: anything already present and verified is skipped.
#
#   tools/fetch_deps.sh
#
# The Android NDK is not downloaded here: set ANDROID_NDK_HOME (the Docker image and
# GitHub's runners provide NDK r27c, 27.2.12479018).
set -euo pipefail
cd "$(dirname "$0")/.."
TP=third_party
mkdir -p "$TP" "$TP/.cache"

die() { echo "fetch_deps: $*" >&2; exit 1; }

# fetch URL SHA256 DEST: download to DEST unless it already has the right hash
fetch() {
    local url=$1 sha=$2 dest=$3
    if [ -f "$dest" ] && echo "$sha  $dest" | sha256sum -c --status; then return; fi
    echo "  downloading $(basename "$dest")"
    mkdir -p "$(dirname "$dest")"
    curl -fsSL --retry 3 -o "$dest.part" "$url" || die "download failed: $url"
    echo "$sha  $dest.part" | sha256sum -c --status || die "checksum mismatch: $url"
    mv "$dest.part" "$dest"
}

# git_at REPO COMMIT DIR: a checkout of exactly COMMIT
git_at() {
    local repo=$1 commit=$2 dir=$3
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then return; fi
    echo "  cloning $(basename "$dir") @ ${commit:0:10}"
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$repo"
    git -C "$dir" fetch -q --depth 1 origin "$commit" || die "fetch failed: $repo $commit"
    git -C "$dir" checkout -q FETCH_HEAD
}

echo "third-party sources:"

# SDL 2.30.9 (zlib license): built from source for Android
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz \
      24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4 "$TP/.cache/SDL2-2.30.9.tar.gz"
[ -f "$TP/SDL2-2.30.9/CMakeLists.txt" ] || tar -xzf "$TP/.cache/SDL2-2.30.9.tar.gz" -C "$TP"

# LZMA SDK 25.01 (public domain): the decoder (runtime) and the encoder (packer)
fetch https://www.7-zip.org/a/lzma2501.7z \
      cbc3babd589d971e45971d787ff100be8aaa5eab15b2694497ec3e447009e1f2 "$TP/.cache/lzma2501.7z"
if [ ! -f "$TP/lzma-enc/LzmaEnc.c" ] || [ ! -f "$TP/lzma/LzmaDec.c" ]; then
    command -v 7z >/dev/null || die "7z is required (p7zip-full)"
    rm -rf "$TP/.cache/lzma" && mkdir -p "$TP/.cache/lzma" "$TP/lzma" "$TP/lzma-enc"
    7z x -y -o"$TP/.cache/lzma" "$TP/.cache/lzma2501.7z" 'C/*' 'DOC/lzma-sdk.txt' >/dev/null
    C="$TP/.cache/lzma/C"
    cp "$C"/{LzmaDec.c,LzmaDec.h,7zTypes.h,7zWindows.h,Compiler.h,Precomp.h} "$TP/lzma/"
    cp "$C"/{LzmaEnc.c,LzmaEnc.h,LzFind.c,LzFind.h,LzFindOpt.c,LzHash.h,CpuArch.c,CpuArch.h,Compiler.h,Precomp.h,7zTypes.h,7zWindows.h,RotateDefs.h} "$TP/lzma-enc/"
    cp "$TP/.cache/lzma/DOC/lzma-sdk.txt" "$TP/lzma-enc/"
fi

# dr_flac 0.12.42 (public domain / MIT-0): FLAC decoding in the pack reader
fetch https://raw.githubusercontent.com/mackron/dr_libs/39ce69188eab79a913aa23423eef9da5f3dcd142/dr_flac.h \
      3c579b159fb3d3dc639f7f5404fa373e515bcd2b6c7616cf0b297debd809ef76 "$TP/dr_libs/dr_flac.h"

# stb (public domain / MIT): TrueType text, Ogg Vorbis
fetch https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h \
      ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab "$TP/stb/stb_truetype.h"
fetch https://raw.githubusercontent.com/nothings/stb/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_vorbis.c \
      4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636 "$TP/stb/stb_vorbis.c"

# Liberation Sans 2.1.5 (SIL OFL 1.1): the Windows UI font stand-in (metric-compatible with Arial)
fetch https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz \
      7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0 "$TP/.cache/liberation-fonts-ttf-2.1.5.tar.gz"
if [ ! -f "$TP/fonts/LiberationSans-Regular.ttf" ]; then
    mkdir -p "$TP/fonts"
    tar -xzf "$TP/.cache/liberation-fonts-ttf-2.1.5.tar.gz" -C "$TP/.cache"
    cp "$TP/.cache/liberation-fonts-ttf-2.1.5/"{LiberationSans-Regular.ttf,LiberationSans-Bold.ttf,LICENSE} "$TP/fonts/"
fi

# TinySoundFont (MIT): the MIDI synthesizer
TSF=https://raw.githubusercontent.com/schellingb/TinySoundFont/853a0a171759f1ddba0de1442133a75912bbeffa
fetch $TSF/tsf.h 70d55963c98f60ebb81518eaa1f25d46888d5180eb5f5289fd6b74ffc177d197 "$TP/tsf/tsf.h"
fetch $TSF/tml.h 93257db259e0efb2ea2037d7157841bec8cb4a2d7986286e43c8090705326546 "$TP/tsf/tml.h"
fetch $TSF/LICENSE ef10cb839de73fb6d5d32ea7151979e87809a6df70dc7e7ee8457ae1c9e7aa7e "$TP/tsf/LICENSE"

# GeneralUser GS 2.0.3 by S. Christian Collins (free to use and redistribute): the General
# MIDI soundfont for the game's music, shipped inside the APK
GU=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a
fetch $GU/GeneralUser-GS.sf2 9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe "$TP/soundfont/GeneralUser-GS.sf2"
fetch $GU/documentation/LICENSE.txt 7b32efefdf95ce38a043799f0659853ddc00fbaa14d8c50f0aca16b9b8b405be "$TP/soundfont/LICENSE.txt"

# LuaJIT 2.1 (MIT): patched at build time for the game's 32-bit memory (tools/luajit_guest32.py)
git_at https://github.com/LuaJIT/LuaJIT.git dd3b24369ddfaa83c78cd6fdd91d157c8968ec8d "$TP/luajit"

# FFmpeg 7.1 (LGPL 2.1+): a minimal static build for the movies and FLAC (tools/build_ffmpeg.sh)
fetch https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz \
      40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6 "$TP/.cache/ffmpeg-7.1.tar.xz"
[ -f "$TP/ffmpeg-7.1/configure" ] || tar -xJf "$TP/.cache/ffmpeg-7.1.tar.xz" -C "$TP"

echo "third-party sources: ok"
