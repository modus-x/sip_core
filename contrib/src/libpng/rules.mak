# libpng
LIBPNG_VERSION := 1.6.54
LIBPNG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libpng-$(LIBPNG_VERSION).tar.gz

PKGS += libpng

DEPS_libpng = zlib

LIBPNG_CMAKECONF = -DPNG_SHARED=OFF \
				-DPNG_FRAMEWORK=OFF

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
