# liblzma
LZMA_VERSION := 5.8.1
LZMA_URL := https://nexus.svetlocal.ru/repository/github-artifacts/liblzma-$(FONTCONFIG_VERSION).tar.gz

PKGS += lzma

$(TARBALLS)/liblzma-$(LZMA_VERSION).tar.gz:
	$(call download,$(LZMA_URL))

.sum-liblzma: liblzma-$(LZMA_VERSION).tar.gz

liblzma: liblzma-$(LZMA_VERSION).tar.gz .sum-liblzma
	$(UNPACK)
	$(MOVE)

.liblzma: liblzma
	cd $< && $(HOSTVARS) ./configure --prefix=$(PREFIX) --static
	cd $< && $(MAKE) install
	touch $@
