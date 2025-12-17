# xml2
LIBXML2_VERSION := 2.14.3
LIBXML2_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libxml2-$(FONTCONFIG_VERSION).tar.gz

PKGS += xml2

$(TARBALLS)/libxml2-$(LIBXML2_VERSION).tar.gz:
	$(call download,$(LIBXML2_URL))

.sum-libxml2: libxml2-$(LIBXML2_VERSION).tar.gz

libxml2: libxml2-$(LIBXML2_VERSION).tar.gz .sum-libxml2
	$(UNPACK)
	$(MOVE)

.libxml2: libxml2
	cd $< && $(HOSTVARS) ./configure --prefix=$(PREFIX) --static
	cd $< && $(MAKE) install
	touch $@
