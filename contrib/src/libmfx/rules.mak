# libmfx
LIBMFX_VERSION := 23.2.2
LIBMFX_URL := https://nexus.svetlocal.ru/repository/github-artifacts/MediaSDK-intel-mediasdk-$(LIBMFX_VERSION).tar.gz

# PKGS += libmfx

DEPS_libmfx = libva

MFX_CMAKECONF = -DBUILD_SHARED_LIBS=OFF \
				-DBUILD_RUNTIME=OFF \
				-DBUILD_SAMPLES=OFF \
				-DBUILD_TUTORIALS=OFF

$(TARBALLS)/MediaSDK-intel-mediasdk-$(LIBMFX_VERSION).tar.gz:
	$(call download,$(LIBMFX_URL))

.sum-libmfx: MediaSDK-intel-mediasdk-$(LIBMFX_VERSION).tar.gz

libmfx: MediaSDK-intel-mediasdk-$(LIBMFX_VERSION).tar.gz .sum-libmfx
	$(UNPACK)
	$(MOVE)

.libmfx: libmfx toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build ${MFX_CMAKECONF}
	cd $</build && $(MAKE) install
	touch $@
