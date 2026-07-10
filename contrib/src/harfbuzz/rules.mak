# harfbuzz
LIBHARFBUZZ_VERSION := 12.2.0
LIBHARFBUZZ_URL := https://nexus.svetlocal.ru/repository/github-artifacts/harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz

PKGS += harfbuzz

DEPS_harfbuzz = freetype2

HBZ_CMAKECONF = -DBUILD_SHARED_LIBS=OFF \
				-DHB_HAVE_FREETYPE=ON \
				-DCMAKE_POSITION_INDEPENDENT_CODE=ON

# iOS: harfbuzz's CMake auto-enables the CoreText backend on any Apple
# target and writes "-framework ApplicationServices" (a macOS-only umbrella
# framework) into harfbuzz.pc; ffmpeg's fontconfig link test then dies with
# "ld: framework 'ApplicationServices' not found". Its IOS branch never
# fires because toolchain.cmake reports CMAKE_SYSTEM_NAME=Darwin. We shape
# through hb-ft (FreeType) everywhere, so drop the CoreText backend on iOS.
ifdef HAVE_IOS
HBZ_CMAKECONF += -DHB_HAVE_CORETEXT=OFF
endif

$(TARBALLS)/harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz:
	$(call download,$(LIBHARFBUZZ_URL))

.sum-harfbuzz: harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz

harfbuzz: harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz .sum-harfbuzz
	$(UNPACK)
	$(MOVE)

.harfbuzz: harfbuzz toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build ${HBZ_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
