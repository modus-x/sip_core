# YAML
YAML_CPP_VERSION := 2f86d13775d119edbb69af52e5f566fd65c6953b
YAML_CPP_URL := https://nexus.svetlocal.ru/repository/github-artifacts/yaml-cpp-$(YAML_CPP_VERSION).tar.gz

PKGS += yaml-cpp

YAML_CPP_CMAKECONF := -DBUILD_STATIC=ON \
                      -DBUILD_SHARED=OFF \
                      -DYAML_CPP_BUILD_TOOLS=OFF \
                      -DYAML_CPP_BUILD_TESTS=OFF \
                      -DYAML_CPP_BUILD_CONTRIB=OFF \
                      -DBUILD_SHARED_LIBS=OFF

$(TARBALLS)/yaml-cpp-$(YAML_CPP_VERSION).tar.gz:
	$(call download,$(YAML_CPP_URL))

.sum-yaml-cpp: yaml-cpp-$(YAML_CPP_VERSION).tar.gz

yaml-cpp: yaml-cpp-$(YAML_CPP_VERSION).tar.gz .sum-yaml-cpp
	$(UNPACK)
	$(MOVE)

.yaml-cpp: yaml-cpp toolchain.cmake
	cd $< && $(HOSTVARS) $(CMAKE) $(YAML_CPP_CMAKECONF)
	cd $< && $(MAKE) install
	touch $@
