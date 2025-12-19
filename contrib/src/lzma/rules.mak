# liblzma
LZMA_VERSION := 5.8.1
LZMA_URL := https://nexus.svetlocal.ru/repository/github-artifacts/liblzma-$(LZMA_VERSION).tar.gz

PKGS += lzma

LZMA_CMAKECONF := -DBUILD_SHARED_LIBS=OFF

$(TARBALLS)/liblzma-$(LZMA_VERSION).tar.gz:
	$(call download,$(LZMA_URL))

.sum-lzma: liblzma-$(LZMA_VERSION).tar.gz

lzma: liblzma-$(LZMA_VERSION).tar.gz .sum-lzma
	$(UNPACK)
	$(MOVE)

.lzma: lzma
	cd $< && $(HOSTVARS) sh autogen.sh --disable-shared --prefix=$(PREFIX)
	cd $< && $(MAKE) install
	touch $@
