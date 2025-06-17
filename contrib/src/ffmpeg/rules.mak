# FFMPEG
FFMPEG_HASH := n5.0
FFMPEG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/ffmpeg-$(FFMPEG_HASH).tar.gz

PKGS+=ffmpeg

DEPS_ffmpeg = iconv zlib vpx opus speex x264

FFMPEGCONF = \
	--cc="$(CC)" \
	--pkg-config="$(PKG_CONFIG)"

#disable everything
FFMPEGCONF += \
	--disable-everything \
	--enable-zlib \
	--enable-gpl \
	--enable-swscale \
	--enable-bsfs \
	--disable-filters \
	--disable-autodetect \
	--disable-programs \
	--disable-postproc \
	--disable-autodetect

FFMPEGCONF += \
	--disable-protocols \
	--enable-protocol=crypto \
	--enable-protocol=file \
	--enable-protocol=rtp \
	--enable-protocol=srtp \
	--enable-protocol=tcp \
	--enable-protocol=udp \
	--enable-protocol=unix \
	--enable-protocol=pipe

#enable muxers/demuxers
FFMPEGCONF += \
	--disable-demuxers \
	--disable-muxers \
	--enable-muxer=rtp \
	--enable-muxer=mp4 \
	--enable-muxer=h264 \
	--enable-muxer=webm \
	--enable-muxer=matroska \
	--enable-muxer=ogg \
	--enable-muxer=pcm_s16be \
	--enable-muxer=pcm_s16le \
	--enable-demuxer=rtp \
	--enable-demuxer=mp3 \
	--enable-demuxer=ogg \
	--enable-demuxer=wav \
	--enable-demuxer=pcm_mulaw \
	--enable-demuxer=pcm_alaw \
	--enable-demuxer=pcm_s16be \
	--enable-demuxer=pcm_s16le \
	--enable-demuxer=h264

#enable parsers
FFMPEGCONF += \
	--enable-parser=h264 \
	--enable-parser=vp8 \
	--enable-parser=vp9 \
	--enable-parser=opus

#encoders/decoders
FFMPEGCONF += \
	--enable-encoder=rawvideo \
	--enable-decoder=rawvideo \
	--enable-encoder=libx264 \
	--enable-decoder=h264 \
	--enable-encoder=pcm_alaw \
	--enable-decoder=pcm_alaw \
	--enable-encoder=pcm_mulaw \
	--enable-decoder=pcm_mulaw \
	--enable-encoder=libvpx_vp8 \
	--enable-encoder=libvpx_vp9 \
	--enable-decoder=vp8 \
	--enable-decoder=vp9 \
	--enable-encoder=h263 \
	--enable-encoder=h263p \
	--enable-decoder=h263 \
	--enable-encoder=mjpeg \
	--enable-decoder=mjpeg \
	--enable-decoder=mjpegb \
	--enable-libspeex \
	--enable-libopus \
	--enable-libvpx \
	--enable-libx264 \
	--enable-encoder=libspeex \
	--enable-decoder=libspeex \
	--enable-encoder=libopus \
	--enable-decoder=libopus

# decoders for ringtones and audio streaming
FFMPEGCONF += \
	--enable-decoder=vorbis \
	--enable-decoder=mp3 \
	--enable-decoder=pcm_u24be \
	--enable-decoder=pcm_u24le \
	--enable-decoder=pcm_u32be \
	--enable-decoder=pcm_u32le \
	--enable-decoder=pcm_u8 \
	--enable-decoder=pcm_f16le \
	--enable-decoder=pcm_f24le \
	--enable-decoder=pcm_f32be \
	--enable-decoder=pcm_f32le \
	--enable-decoder=pcm_f64be \
	--enable-decoder=pcm_f64le \
	--enable-decoder=pcm_s16be \
	--enable-decoder=pcm_s16be_planar \
	--enable-decoder=pcm_s16le \
	--enable-decoder=pcm_s16le_planar \
	--enable-decoder=pcm_s24be \
	--enable-decoder=pcm_s24le \
	--enable-decoder=pcm_s24le_planar \
	--enable-decoder=pcm_s32be \
	--enable-decoder=pcm_s32le \
	--enable-decoder=pcm_s32le_planar \
	--enable-decoder=pcm_s64be \
	--enable-decoder=pcm_s64le \
	--enable-decoder=pcm_s8 \
	--enable-decoder=pcm_s8_planar \
	--enable-decoder=pcm_u16be \
	--enable-decoder=pcm_u16le

# filters
FFMPEGCONF += \
	--enable-filter=scale \
	--enable-filter=overlay \
	--enable-filter=amix \
	--enable-filter=amerge \
	--enable-filter=aresample \
	--enable-filter=format \
	--enable-filter=aformat \
	--enable-filter=fps \
	--enable-filter=transpose \
	--enable-filter=pad

# platform specific options (LINUX / MAC)
ifdef HAVE_LINUX
FFMPEGCONF += --enable-pic --disable-asm


ifdef HAVE_ANDROID
# Android Linux
FFMPEGCONF += \
	--target-os=android \
	--enable-jni \
	--enable-decoder=vp8_mediacodec \
	--enable-mediacodec \
	--disable-vulkan \
	--enable-decoder=h264_mediacodec \
	--enable-cross-compile \
	--ranlib=$(RANLIB) \
	--strip=$(STRIP) \
	--cc=$(CC) \
	--cxx=$(CXX) \
	--ld=$(CC) \
	--ar=$(AR)
# ASM not working on Android x86 https://trac.ffmpeg.org/ticket/4928
ifeq ($(ARCH),i386)
FFMPEGCONF += --disable-asm
endif
ifeq ($(ARCH),x86_64)
FFMPEGCONF += --disable-asm
endif

else

# Desktop Linux
FFMPEGCONF += \
	--target-os=linux \
	--enable-indev=v4l2 \
	--enable-indev=xcbgrab \
	--enable-libxcb \
	--enable-libxcb-shm \
	--enable-libxcb-xfixes \
	--enable-libxcb-shape
# End Desktop Linux:
endif

# End HAVE_LINUX:
endif

ifdef HAVE_MACOSX
FFMPEGCONF += \
	--enable-avfoundation \
	--enable-indev=avfoundation \
	--enable-videotoolbox \
	--enable-hwaccel=h263_videotoolbox \
	--enable-hwaccel=h264_videotoolbox \
	--enable-hwaccel=mpeg4_videotoolbox \
	--enable-hwaccel=hevc_videotoolbox \
	--enable-encoder=h264_videotoolbox \
	--enable-encoder=hevc_videotoolbox \
	--disable-securetransport
endif

ifdef HAVE_IOS
FFMPEGCONF += \
	--enable-videotoolbox \
	--enable-hwaccel=h263_videotoolbox \
	--enable-hwaccel=h264_videotoolbox \
	--enable-hwaccel=mpeg4_videotoolbox \
	--enable-hwaccel=hevc_videotoolbox \
	--enable-encoder=h264_videotoolbox \
	--enable-encoder=hevc_videotoolbox \
	--target-os=darwin \
	--enable-cross-compile \
	--enable-pic
endif

# x86 stuff
ifeq ($(ARCH),i386)
FFMPEGCONF += --arch=x86
endif

ifeq ($(ARCH),x86_64)
FFMPEGCONF += --arch=x86_64
endif

# ARM stuff
ifeq ($(ARCH),arm)
FFMPEGCONF += --arch=arm
ifdef HAVE_ARMV7A
FFMPEGCONF += --cpu=cortex-a8
endif
ifdef HAVE_ARMV6
FFMPEGCONF += --cpu=armv6 --disable-neon
endif
endif

# ARM64 stuff
ifeq ($(ARCH),aarch64)
FFMPEGCONF += --arch=aarch64
endif
ifeq ($(ARCH),arm64)
FFMPEGCONF += --arch=aarch64
endif
ifeq ($(ARCH),armv7a)
FFMPEGCONF += --arch=arm --enable-neon --enable-armv6 --enable-vfpv3
endif

$(TARBALLS)/ffmpeg-$(FFMPEG_HASH).tar.gz:
	$(call download,$(FFMPEG_URL))

.sum-ffmpeg: ffmpeg-$(FFMPEG_HASH).tar.gz

ffmpeg: ffmpeg-$(FFMPEG_HASH).tar.gz
	rm -Rf $@ $@-$(FFMPEG_HASH)
	mkdir -p $@-$(FFMPEG_HASH)
	(cd $@-$(FFMPEG_HASH) && tar x $(if ${BATCH_MODE},,-v) --strip-components=1 -f $<)
	$(APPLY) $(SRC)/ffmpeg/remove-mjpeg-log.patch
	$(APPLY) $(SRC)/ffmpeg/change-RTCP-ratio.patch
	$(APPLY) $(SRC)/ffmpeg/rtp_ext_abs_send_time.patch
	$(APPLY) $(SRC)/ffmpeg/rtp_any_payload.patch
	$(APPLY) $(SRC)/ffmpeg/rtp_dtmf.patch
	$(APPLY) $(SRC)/ffmpeg/libopusdec-enable-FEC.patch
	$(APPLY) $(SRC)/ffmpeg/libopusenc-reload-packet-loss-at-encode.patch
	$(APPLY) $(SRC)/ffmpeg/ios-disable-b-frames.patch
	$(APPLY) $(SRC)/ffmpeg/screen-sharing-x11-fix.patch
	$(UPDATE_AUTOCONFIG)
	$(MOVE)

.ffmpeg: ffmpeg .sum-ffmpeg
	cd $< && $(HOSTVARS) ./configure \
		--extra-cflags="$(CFLAGS)" \
		--extra-ldflags="$(LDFLAGS)" $(FFMPEGCONF) \
		--prefix="$(PREFIX)" --enable-static --disable-shared \
                --pkg-config-flags="--static"
	cd $< && $(MAKE) install-libs install-headers
	touch $@