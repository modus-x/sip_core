# libvpl
LIBVPL_VERSION := 2.16.0
LIBVPL_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libvpl-$(LIBVPL_VERSION).tar.gz

PKGS += libvpl

# DEPS_libvpl = libvpl_gpu

VPL_CMAKECONF = -DBUILD_SHARED_LIBS=OFF \
				-DINSTALL_LIB=ON \
				-DCXX_LIB=-lstdc++ \
				-DCMAKE_INSTALL_PREFIX=$(PREFIX)

$(TARBALLS)/libvpl-$(LIBVPL_VERSION).tar.gz:
	$(call download,$(LIBVPL_URL))

.sum-libvpl: libvpl-$(LIBVPL_VERSION).tar.gz

libvpl: libvpl-$(LIBVPL_VERSION).tar.gz .sum-libvpl
	$(UNPACK)
	$(MOVE)

.libvpl: libvpl toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build  ${VPL_CMAKECONF}
	cd $< && cmake --build build
	cd $< && cmake --install build
	touch $@
