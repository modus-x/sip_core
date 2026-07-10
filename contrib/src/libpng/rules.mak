# libpng
LIBPNG_VERSION := 1.6.54
LIBPNG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libpng-$(LIBPNG_VERSION).tar.gz

PKGS += libpng

# force the usage of system package in linux
ifdef HAVE_LINUX
PKGS_FOUND += libpng
endif

DEPS_libpng = zlib

LIBPNG_CMAKECONF = -DPNG_SHARED=OFF \
				-DPNG_FRAMEWORK=OFF

# iOS: libpng's pnglibconf generation preprocesses DFA files with
# ${CMAKE_C_COMPILER} directly (scripts/cmake/genout.cmake), which explodes
# when CC is the multi-word "xcrun clang" from main.mak — only "xcrun"
# survives into the custom command and it chokes on -E. Upstream's own
# CMakeLists declares a prebuilt PNG_LIBCONF_HEADER "mandatory" for iOS, but
# our toolchain.cmake reports CMAKE_SYSTEM_NAME=Darwin so the automatic
# `if(ANDROID OR IOS)` branch never triggers. Force the prebuilt header
# explicitly ($$PWD is the libpng source dir at recipe time via `cd $<`).
ifdef HAVE_IOS
LIBPNG_CMAKECONF += -DPNG_LIBCONF_HEADER=$$PWD/scripts/pnglibconf.h.prebuilt
# Our toolchain.cmake reports CMAKE_SYSTEM_NAME=Darwin with no
# CMAKE_SYSTEM_PROCESSOR / CMAKE_OSX_ARCHITECTURES, so libpng's
# PNG_TARGET_ARCHITECTURE comes out empty and its ARM branch never adds the
# NEON sources — while pngpriv.h still auto-enables PNG_ARM_NEON_OPT from
# __ARM_NEON, leaving _png_*_neon symbols undefined in libpng16.a (first
# seen as a failed ffmpeg fontconfig link test). Both iOS targets are
# arm64-only, so pin the architecture; NEON sources then build with
# PNG_ARM_NEON_OPT=2, matching the header.
LIBPNG_CMAKECONF += -DCMAKE_OSX_ARCHITECTURES=arm64
endif

$(TARBALLS)/libpng-$(LIBPNG_VERSION).tar.gz:
	$(call download,$(LIBPNG_URL))

.sum-libpng: libpng-$(LIBPNG_VERSION).tar.gz

libpng: libpng-$(LIBPNG_VERSION).tar.gz .sum-libpng
	$(UNPACK)
	$(MOVE)

.libpng: libpng toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build ${LIBPNG_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
