# freetype
FREETYPE_VERSION := VER-2-14-1
FREETYPE_URL := https://nexus.svetlocal.ru/repository/github-artifacts/freetype-$(FREETYPE_VERSION).tar.gz

PKGS += freetype

$(TARBALLS)/freetype-$(FREETYPE_VERSION).tar.gz:
	$(call download,$(FREETYPE_URL))

.sum-freetype: zlib-$(FREETYPE_VERSION).tar.gz

freetype: freetype-$(FREETYPE_VERSION).tar.gz .sum-freetype
	$(UNPACK)
	$(MOVE)

.freetype: freetype
	cd $< && $(HOSTVARS) ./configure --prefix=$(PREFIX) --static
	cd $< && $(MAKE) install
	touch $@
