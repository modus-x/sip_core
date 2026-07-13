# opencl-icd
OPENCL_ICD_VERSION := 2025.07.22
OPENCL_ICD_URL := https://nexus.svetlocal.ru/repository/github-artifacts/OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz

DEPS_opencl-icd += opencl-headers

# Static: a shared ICD loader would add a runtime libOpenCL dependency that
# nothing ships; the loader still dlopens vendor ICDs at runtime.
OPENCL_ICD_CMAKECONF = -DCMAKE_PREFIX_PATH=$(PREFIX) -DBUILD_SHARED_LIBS=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON

$(TARBALLS)/OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz:
	$(call download,$(OPENCL_ICD_URL))

.sum-opencl-icd: OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz

opencl-icd: OpenCL-ICD-Loader-$(OPENCL_ICD_VERSION).tar.gz .sum-opencl-icd
	$(UNPACK)
	$(MOVE)

.opencl-icd: opencl-icd toolchain.cmake
	cd $< && $(HOSTVARS) cmake -E make_directory build && $(CMAKE) -B build ${OPENCL_ICD_CMAKECONF}
	cd $</build && cmake --build . --target install
	# static ICD loader dlopens vendor drivers at runtime; consumers linking
	# via pkg-config --static need these on the link line
	grep -q 'Libs.private' $(PREFIX)/lib/pkgconfig/OpenCL.pc || \
		printf 'Libs.private: -ldl -lpthread\n' >> $(PREFIX)/lib/pkgconfig/OpenCL.pc
	touch $@
