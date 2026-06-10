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
