# opencl-headers
OPENCL_HEADERS_VERSION := 2025.07.22
OPENCL_HEADERS_URL := https://nexus.svetlocal.ru/repository/github-artifacts/OpenCL-Headers-$(OPENCL_HEADERS_VERSION).tar.gz

$(TARBALLS)/OpenCL-Headers-$(OPENCL_HEADERS_VERSION).tar.gz:
	$(call download,$(OPENCL_HEADERS_URL))

.sum-opencl-headers: OpenCL-Headers-$(OPENCL_HEADERS_VERSION).tar.gz

opencl-headers: OpenCL-Headers-$(OPENCL_HEADERS_VERSION).tar.gz .sum-opencl-headers
	$(UNPACK)
	$(MOVE)

.opencl-headers: opencl-headers toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build
	cd $</build && cmake --build . --target install
	# cmake installs the .pc into share/pkgconfig, which the contrib
	# PKG_CONFIG_PATH does not search — mirror it into lib/pkgconfig so
	# OpenCL.pc's "Requires: OpenCL-Headers" resolves (ffmpeg configure).
	mkdir -p $(PREFIX)/lib/pkgconfig
	cp -f $(PREFIX)/share/pkgconfig/OpenCL-Headers.pc $(PREFIX)/lib/pkgconfig/
	touch $@
