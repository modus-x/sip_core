#!/usr/bin/env bash
set +x
set +e

cd $3/ffmpeg

INSTALL_DIR="${2//\\//}"

INCLUDE_DIR="${INSTALL_DIR}/include"
LIB_DIR="${INSTALL_DIR}/lib/x64"

echo include dir is $INCLUDE_DIR
echo lib dir is $LIB_DIR

FFMPEGCONF='
            --toolchain=msvc
            --target-os=win32'

#disable everything
FFMPEGCONF+='
            --disable-everything
            --disable-programs
            --disable-d3d11va
            --disable-dxva2
            --disable-postproc
            --disable-filters'

FFMPEGCONF+='
            --enable-static
            --disable-shared
            --enable-cross-compile
            --enable-gpl
            --enable-swscale
            --enable-protocols
            --enable-bsfs'

#enable muxers/demuxers
FFMPEGCONF+='
            --enable-demuxers
            --enable-muxers'

#enable parsers
FFMPEGCONF+='
            --enable-parser=h264
            --enable-parser=vp8
            --enable-parser=vp9
            --enable-parser=opus'

#encoders/decoders
FFMPEGCONF+='
            --enable-libvpx
            --enable-encoder=libvpx_vp8
            --enable-decoder=vp8 
            --enable-decoder=vp9 
            --enable-libopus
            --enable-encoder=libopus
            --enable-decoder=libopus
            --enable-encoder=pcm_alaw
            --enable-decoder=pcm_alaw
            --enable-encoder=pcm_mulaw
            --enable-decoder=pcm_mulaw
            --enable-libx264
            --enable-encoder=libx264
            --enable-decoder=h264
            --enable-encoder=rawvideo
            --enable-decoder=rawvideo'

# decoders for ringtones and audio streaming
FFMPEGCONF+='
            --enable-decoder=vorbis
            --enable-decoder=ac3
            --enable-decoder=eac3
            --enable-decoder=mp3
            --enable-decoder=pcm_u24be
            --enable-decoder=pcm_u24le
            --enable-decoder=pcm_u32be
            --enable-decoder=pcm_u32le
            --enable-decoder=pcm_u8
            --enable-decoder=pcm_f16le
            --enable-decoder=pcm_f24le
            --enable-decoder=pcm_f32be
            --enable-decoder=pcm_f32le
            --enable-decoder=pcm_f64be
            --enable-decoder=pcm_f64le
            --enable-decoder=pcm_s16be
            --enable-decoder=pcm_s16be_planar
            --enable-decoder=pcm_s16le
            --enable-decoder=pcm_s16le_planar
            --enable-decoder=pcm_s24be
            --enable-decoder=pcm_s24le
            --enable-decoder=pcm_s24le_planar
            --enable-decoder=pcm_s32be
            --enable-decoder=pcm_s32le
            --enable-decoder=pcm_s32le_planar
            --enable-decoder=pcm_s64be
            --enable-decoder=pcm_s64le
            --enable-decoder=pcm_s8
            --enable-decoder=pcm_s8_planar
            --enable-decoder=pcm_u16be
            --enable-decoder=pcm_u16le'

#filters
FFMPEGCONF+='
            --enable-filter=scale
            --enable-filter=overlay
            --enable-filter=amix
            --enable-filter=amerge
            --enable-filter=aresample
            --enable-filter=format
            --enable-filter=aformat
            --enable-filter=fps
            --enable-filter=transpose
            --enable-filter=pad'

FFMPEGCONF+='
                --enable-indev=dshow
                --enable-indev=gdigrab'

echo "configure and make ffmpeg for win32-x64... in $(pwd)"

# extra libs
EXTRALDFLAGS="libopus.lib libx264.lib libvpx.lib"

# configure debug / release libs
if [ "$1" == "Debug" ]; then
  EXTRACFLAGS="-MDd"
  FFMPEGCONF+=' --enable-debug --disable-optimizations'
  # IGNORE LIBCMT -> read https://trac.ffmpeg.org/wiki/CompilationGuide/MSVC#DebugBuilds
  EXTRALDFLAGS=" ${EXTRALDFLAGS} /NODEFAULTLIB:libcmt"
else
  EXTRACFLAGS="-MD"
fi

EXTRACFLAGS="${EXTRACFLAGS} -D_WINDLL -D_WIN32_WINNT=0x0601 -I${INCLUDE_DIR} -I${INCLUDE_DIR}/opus"

EXTRALDFLAGS="${EXTRALDFLAGS} -APPCONTAINER:NO -MACHINE:x64 Ole32.lib Kernel32.lib Gdi32.lib User32.lib Strmiids.lib Advapi32.lib OleAut32.lib Shlwapi.lib Vfw32.lib Secur32.lib Advapi32.lib -LIBPATH:${LIB_DIR}"
FFMPEGCONF+=' --arch=x86_64'

# DO NOT mix debug and release builds
OUTDIR=Output/win32/x64/$1
mkdir -p $OUTDIR
cd $OUTDIR
pwd

FFMPEGCONF=$(echo $FFMPEGCONF | sed -e "s/[[:space:]]\+/ /g")

set -x
set -e
../../../../configure $FFMPEGCONF --extra-cflags="${EXTRACFLAGS}" --extra-ldflags="${EXTRALDFLAGS}" --prefix="${INSTALL_DIR}" --extra-cxxflags="-std:c++20"
make -j8 install
cd ../../../..
