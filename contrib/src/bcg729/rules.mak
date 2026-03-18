# bcg729
BCG729_VERSION := 1.1.1
BCG729_URL := https://nexus.svetlocal.ru/repository/github-artifacts/bcg729-$(BCG729_VERSION).tar.gz

PKGS += bcg729

BCG729_CMAKECONF := -DENABLE_STATIC=ON \
                    -DENABLE_SHARED=OFF \
					-DENABLE_TESTS=NO \
					-DCMAKE_POLICY_VERSION_MINIMUM=3.5

$(TARBALLS)/bcg729-$(BCG729_VERSION).tar.gz:
	$(call download,$(BCG729_URL))

.sum-bcg729: bcg729-$(BCG729_VERSION).tar.gz

bcg729: bcg729-$(BCG729_VERSION).tar.gz .sum-bcg729
	$(UNPACK)
	$(MOVE)

.bcg729: bcg729 toolchain.cmake
	cd $< && $(HOSTVARS) $(CMAKE) $(BCG729_CMAKECONF)
	cd $< && $(MAKE) install
	touch $@
