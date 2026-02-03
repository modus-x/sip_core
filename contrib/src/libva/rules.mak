# libva
LIBVA_VERSION := 2.23.0
LIBVA_URL := https://nexus.svetlocal.ru/repository/github-artifacts/libva-$(LIBVA_VERSION).tar.gz

PKGS += libva

$(TARBALLS)/libva-$(LIBVA_VERSION).tar.gz:
	$(call download,$(LIBVA_URL))

.sum-libva: libva-$(LIBVA_VERSION).tar.gz

libva: libva-$(LIBVA_VERSION).tar.gz .sum-libva
	$(UNPACK)
	$(APPLY) $(SRC)/libva/libva.pc.in.patch
	$(MOVE)

.libva: libva
	cd $< && $(HOSTVARS) sh autogen.sh $(HOSTCONF) --with-drivers-path=="/usr/lib/$(HOST)/dri/" --enable-drm USE_X11_FALSE USE_WAYLAND_FALSE USE_GLX_FALSE DRM_LIBS="-ldrm"
	cd $< && $(MAKE) install
	touch $@
