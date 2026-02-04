# fontconfig
FONTCONFIG_VERSION := 2.16.2
FONTCONFIG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/fontconfig-$(FONTCONFIG_VERSION).tar.gz

PKGS += fontconfig

DEPS_fontconfig = freetype2 iconv xml2

ifdef HAVE_MACOSX
# on macos, we must install it via brew
HOSTVARS += LIBTOOLIZE=glibtoolize
endif

# When targeting Darwin (especially cross-arch on macOS), skip utility subdirs
# so install does not try to link target executables like fc-cache.
FONTCONFIG_INSTALL_SUBDIRS := \
	SUBDIRS='fontconfig fc-case fc-lang src conf.d'

$(TARBALLS)/fontconfig-$(FONTCONFIG_VERSION).tar.gz:
	$(call download,$(FONTCONFIG_URL))

.sum-fontconfig: fontconfig-$(FONTCONFIG_VERSION).tar.gz

fontconfig: fontconfig-$(FONTCONFIG_VERSION).tar.gz .sum-fontconfig
	$(UNPACK)
	$(APPLY) $(SRC)/fontconfig/configure.ac.patch
	$(MOVE)

.fontconfig: fontconfig
	cd $< && $(HOSTVARS) sh autogen.sh $(HOSTCONF) --disable-docs --enable-static \
		--disable-shared --disable-cache-build --disable-docbook
	cd $< && $(HOSTVARS) $(MAKE) $(FONTCONFIG_INSTALL_SUBDIRS) install
	touch $@
