# fontconfig
FONTCONFIG_VERSION := 2.16.2
FONTCONFIG_URL := https://nexus.svetlocal.ru/repository/github-artifacts/fontconfig-$(FONTCONFIG_VERSION).tar.gz

PKGS += fontconfig

DEPS_fontconfig = freetype2 iconv xml2

# Any Darwin-family target (macOS desktop AND iOS device/simulator) is built
# on a macOS host, where GNU libtoolize is Homebrew's glibtoolize. The old
# HAVE_MACOSX guard missed iOS targets, whose autogen.sh then failed with
# "You must have libtool 1.4 installed to compile Fontconfig."
ifdef HAVE_DARWIN_OS
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
		--disable-shared --disable-cache-build --disable-docbook --with-pic
	cd $< && $(HOSTVARS) $(MAKE) $(FONTCONFIG_INSTALL_SUBDIRS) install
	touch $@
