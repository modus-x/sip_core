# xml2
LIBXML2_VERSION := 2.14.3
LIBXML2_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libxml2-$(LIBXML2_VERSION).tar.gz

PKGS += xml2

DEPS_xml2 = zlib iconv

XML2_CMAKECONF := -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_PYTHON=OFF

$(TARBALLS)/libxml2-$(LIBXML2_VERSION).tar.gz:
	$(call download,$(LIBXML2_URL))

.sum-xml2: libxml2-$(LIBXML2_VERSION).tar.gz

xml2: libxml2-$(LIBXML2_VERSION).tar.gz .sum-xml2
	$(UNPACK)
	$(MOVE)

.xml2: xml2 toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build &&  $(CMAKE) -B build ${XML2_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
