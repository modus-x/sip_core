# ffnvcodec
# 9.1.23.3 for ffmpeg 5.0
FFNVCODEC_VERSION := n12.2.72.0
FFNVCODEC_URL := https://nexus.svetlocal.ru/repository/github-artifacts/nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz

$(TARBALLS)/nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz:
	$(call download,$(FFNVCODEC_URL))

.sum-ffnvcodec: nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz

ffnvcodec: nv-codec-headers-$(FFNVCODEC_VERSION).tar.gz .sum-ffnvcodec
	$(UNPACK)
	$(MOVE)

.ffnvcodec: ffnvcodec
	# absolute PREFIX (no DESTDIR): with PREFIX="" the generated ffnvcodec.pc
	# had an empty prefix (-I/include), so ffmpeg's cuda check failed
	cd $< && $(HOSTVARS) $(MAKE) install PREFIX="$(PREFIX)"
	touch $@
