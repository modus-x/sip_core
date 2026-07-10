# brotli
BROTLI_VERSION := 1.2.0
BROTLI_URL := https://nexus.svetlocal.ru/repository/github-artifacts/brotli-$(BROTLI_VERSION).tar.gz

PKGS += brotli

BROTLI_CMAKECONF = -DBUILD_SHARED_LIBS=OFF \
				-DBROTLI_BUILD_TOOLS=OFF

$(TARBALLS)/brotli-$(BROTLI_VERSION).tar.gz:
	$(call download,$(BROTLI_URL))

.sum-brotli: brotli-$(BROTLI_VERSION).tar.gz

brotli: brotli-$(BROTLI_VERSION).tar.gz .sum-brotli
	$(UNPACK)
	$(MOVE)

.brotli: brotli toolchain.cmake
	cd $< && cmake -E make_directory build && $(CMAKE) -B build ${BROTLI_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
