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
	touch $@
