# opencl-icd
OPENCL_ICD_VERSION := 2025.07.22
OPENCL_ICD_URL := https://nexus.svetlocal.ru/repository/github-artifacts/OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz

DEPS_opencl-icd += opencl-headers

OPENCL_ICD_CMAKECONF = -DCMAKE_PREFIX_PATH=$(INSTALL_PREFIX)

$(TARBALLS)/OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz:
	$(call download,$(OPENCL_ICD_URL))

.sum-opencl-icd: OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz

opencl-icd: OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz .sum-opencl-icd
	$(UNPACK)
	$(MOVE)

.opencl-icd: opencl-icd toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build ${OPENCL_ICD_CMAKECONF}
	cd $</build && cmake --build . --target install
	touch $@
