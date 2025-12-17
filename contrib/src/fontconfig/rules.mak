# fontconfig
FONTCONFIG_VERSION := 2-16-2
FONTCONFIG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/fontconfig-$(FONTCONFIG_VERSION).tar.gz

PKGS += fontconfig

$(TARBALLS)/fontconfig-$(FONTCONFIG_VERSION).tar.gz:
	$(call download,$(FONTCONFIG_URL))

.sum-fontconfig: fontconfig-$(FONTCONFIG_VERSION).tar.gz

fontconfig: fontconfig-$(FONTCONFIG_VERSION).tar.gz .sum-fontconfig
	$(UNPACK)
	$(MOVE)

.fontconfig: fontconfig
	cd $< && $(HOSTVARS) ./configure --prefix=$(PREFIX)
	cd $< && $(MAKE) install
	touch $@
