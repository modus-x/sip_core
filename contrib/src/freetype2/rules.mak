# freetype2
FREETYPE_VERSION := VER-2-13-3
FREETYPE_URL := https://nexus.svetlocal.ru/repository/github-artifacts/freetype2-$(FREETYPE_VERSION).tar.gz

PKGS += freetype2

DEPS_freetype2 = libpng zlib brotli

FREETYPE_CMAKECONF := -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON

$(TARBALLS)/freetype2-$(FREETYPE_VERSION).tar.gz:
	$(call download,$(FREETYPE_URL))

.sum-freetype2: freetype2-$(FREETYPE_VERSION).tar.gz

freetype2: freetype2-$(FREETYPE_VERSION).tar.gz .sum-freetype2
	$(UNPACK)
	$(MOVE)

.freetype2: freetype2 toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build &&  $(CMAKE) -B build  ${FREETYPE_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
