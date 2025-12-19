# fontconfig
FONTCONFIG_VERSION := 2.16.2
FONTCONFIG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/fontconfig-$(FONTCONFIG_VERSION).tar.gz

PKGS += fontconfig

DEPS_fontconfig = freetype2 iconv xml2

$(TARBALLS)/fontconfig-$(FONTCONFIG_VERSION).tar.gz:
	$(call download,$(FONTCONFIG_URL))

.sum-fontconfig: fontconfig-$(FONTCONFIG_VERSION).tar.gz

fontconfig: fontconfig-$(FONTCONFIG_VERSION).tar.gz .sum-fontconfig
	$(UNPACK)
	$(APPLY) $(SRC)/fontconfig/configure.patch
	$(MOVE)

.fontconfig: fontconfig
	cd $< && $(HOSTVARS) sh autogen.sh $(HOSTCONF) --disable-docs
	cd $< && $(MAKE) install
	touch $@
