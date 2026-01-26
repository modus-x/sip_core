# libvdpau
LIBVDPAU_VERSION := 1.5
LIBVDPAU_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libvdpau-$(LIBVDPAU_VERSION).tar.gz

PKGS += libvdpau

MESON_HOSTCONF := $(BUILD)
MESON_HOSTCONF += --prefix="$(PREFIX)"
MESON_HOSTCONF += --datadir="$(PREFIX)/share"
MESON_HOSTCONF += --includedir="$(PREFIX)/include"
MESON_HOSTCONF += --libdir="$(PREFIX)/lib"

$(TARBALLS)/libvdpau-$(LIBVDPAU_VERSION).tar.gz:
	$(call download,$(LIBVDPAU_URL))

.sum-libvdpau: libvdpau-$(LIBVDPAU_VERSION).tar.gz

libvdpau: libvdpau-$(LIBVDPAU_VERSION).tar.gz .sum-libvdpau
	$(UNPACK)
	$(MOVE)

.libvdpau: libvdpau 
	cd $< && $(HOSTVARS) meson setup $(MESON_HOSTCONF)
	cd $</$(BUILD) && ninja
	cd $</$(BUILD) && ninja install
	touch $@
