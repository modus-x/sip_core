# harfbuzz
LIBHARFBUZZ_VERSION := 12.2.0
LIBHARFBUZZ_URL := https://nexus.svetlocal.ru/repository/github-artifacts/harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz

PKGS += harfbuzz

HBZ_CMAKECONF = -DBUILD_SHARED_LIBS=Off \
				-DHB_HAVE_FREETYPE=ON

$(TARBALLS)/harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz:
	$(call download,$(LIBHARFBUZZ_URL))

.sum-harfbuzz: harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz

harfbuzz: harfbuzz-$(LIBHARFBUZZ_VERSION).tar.gz .sum-harfbuzz
	$(UNPACK)
	$(MOVE)

.harfbuzz: harfbuzz toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build ${HBZ_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
