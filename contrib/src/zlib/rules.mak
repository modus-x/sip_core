# ZLIB
ZLIB_VERSION := 1.3.1
ZLIB_URL := https://nexus.svetlocal.ru/repository/github-artifacts/zlib-$(ZLIB_VERSION).tar.gz

PKGS += zlib
ifeq ($(shell uname),Darwin) # zlib tries to use libtool on Darwin

ifdef HAVE_CROSS_COMPILE
ZLIB_CONFIG_VARS=CHOST=$(HOST)
endif
endif

$(TARBALLS)/zlib-$(ZLIB_VERSION).tar.gz:
	$(call download,$(ZLIB_URL))

.sum-zlib: zlib-$(ZLIB_VERSION).tar.gz

zlib: zlib-$(ZLIB_VERSION).tar.gz .sum-zlib
	$(UNPACK)
	$(MOVE)

.zlib: zlib
	cd $< && $(HOSTVARS) $(ZLIB_CONFIG_VARS) ./configure --prefix=$(PREFIX) --static
	cd $< && $(MAKE) install
	touch $@
