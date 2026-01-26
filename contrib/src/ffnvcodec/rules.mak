# ffnvcodec
# 9.1.23.3 for ffmpeg 5.0
FFNVCODEC_VERSION := n12.2.72.0
FFNVCODEC_URL := https://nexus.svetlocal.ru/repository/github-artifacts/nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz

PKGS_FOUND += ffnvfcodec

$(TARBALLS)/nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz:
	$(call download,$(FFNVCODEC_URL))

.sum-ffnvcodec: nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz

ffnvcodec: nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz .sum-ffnvcodec
	$(UNPACK)
	$(MOVE)

.ffnvcodec: ffnvcodec
	cd $< && $(HOSTVARS) DESTDIR=$(PREFIX) $(MAKE) install PREFIX=""
	touch $@
