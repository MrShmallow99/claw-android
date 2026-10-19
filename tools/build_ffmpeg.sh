#!/bin/bash
# usage: tools/build_ffmpeg.sh host|android|android-x86_64 -- minimal static FFmpeg (decoders for the game's
# Smacker and AVI/Xvid+MP3 movies, plus swscale) -> build-ffmpeg-<target>/{include,lib}
set -e
cd "$(dirname "$0")/.."
T=${1:-host}
ROOT=$(pwd)
OUT=$ROOT/build-ffmpeg-$T
rm -rf $OUT/obj && mkdir -p $OUT/obj && cd $OUT/obj
COMMON="--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
  --disable-avdevice --disable-avfilter --disable-postproc --enable-static --disable-shared --enable-pic
  --enable-avformat --enable-avcodec --enable-swscale --enable-swresample
  --enable-demuxer=avi --enable-demuxer=smacker
  --enable-decoder=mpeg4 --enable-decoder=mp3 --enable-decoder=mp3float --enable-decoder=smacker --enable-decoder=smackaud
  --enable-encoder=flac --enable-decoder=pcm_u8 --enable-decoder=pcm_s16le --enable-decoder=adpcm_ima_wav --enable-decoder=msvideo1 --enable-decoder=cinepak
  --enable-parser=mpeg4video --enable-parser=mpegaudio --prefix=$OUT"
if [ "${T%%-*}" = android ]; then
  NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK:-$ROOT/third_party/android-ndk-r27c}}
  B=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
  ARCH=aarch64; TRIPLE=aarch64-linux-android24; EXTRA=""
  [ "$T" = android-x86_64 ] && { ARCH=x86_64; TRIPLE=x86_64-linux-android24; EXTRA="--disable-asm --disable-inline-asm"; }
  $ROOT/third_party/ffmpeg-7.1/configure $COMMON $EXTRA --enable-cross-compile --target-os=android --arch=$ARCH \
    --cc=$B/$TRIPLE-clang --cxx=$B/$TRIPLE-clang++ --ar=$B/llvm-ar \
    --nm=$B/llvm-nm --ranlib=$B/llvm-ranlib --strip=$B/llvm-strip > /dev/null
else
  $ROOT/third_party/ffmpeg-7.1/configure $COMMON --disable-x86asm > /dev/null
fi
make -j$(nproc) > /dev/null && make install > /dev/null
rm -rf $OUT/obj
ls $OUT/lib
