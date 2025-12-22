# liblzma
LZMA_VERSION := 5.8.1
LZMA_URL := https://nexus.svetlocal.ru/repository/github-artifacts/liblzma-$(LZMA_VERSION).tar.gz

#PKGS += lzma

LZMA_CMAKECONF := -DBUILD_SHARED_LIBS=OFF

$(TARBALLS)/liblzma-$(LZMA_VERSION).tar.gz:
	$(call download,$(LZMA_URL))

.sum-lzma: liblzma-$(LZMA_VERSION).tar.gz

lzma: liblzma-$(LZMA_VERSION).tar.gz .sum-lzma
	$(UNPACK)
	$(MOVE)
	cd $< && $(APPLY) SMP/SMP.patch

.lzma: lzma
	cd $< && $(HOSTVARS) cmake -E make_directory build &&  $(CMAKE) -B build  ${LZMA_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@

