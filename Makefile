LIBRARY = lib/krosshair.so

CCFLAGS = -Wall -std=c99 -fPIC -shared -I./include/
LDFLAGS = -ldl -lm

# Debug build by default; use `make release` or `make RELEASE=1` for release
ifdef RELEASE
CCFLAGS += -O2 -DNDEBUG
else
CCFLAGS += -ggdb
endif

SOURCES = $(shell find src -type f -name "*.c")
# test_mock_icd.c is a standalone app linked against libvulkan (it runs
# through the real loader), not a unit test compiled with the layer sources.
TEST_SOURCES = $(filter-out tests/test_mock_icd.c,$(wildcard tests/test_*.c))
MOCK_ICD_DIR = $(BUILD_DIR)/mock_icd
MOCK_ICD_SO  = $(MOCK_ICD_DIR)/libmock_icd.so

# Test builds link the whole layer with UNIT_TEST defined, which exports a
# few internal symbols (see tests/test_layer_api.h).
TEST_CCFLAGS = -Wall -std=c99 -ggdb -DUNIT_TEST -I./include/ -I./tests

BUILD_DIR = build
FLATPAK_BUILD_DIR = $(BUILD_DIR)/flatpak
FLATPAK_BUILD_DIR_INTERMEDIATE = $(FLATPAK_BUILD_DIR)/intermediate
FLATPAK_EXPORT_DIR = $(FLATPAK_BUILD_DIR_INTERMEDIATE)/export

FLATPAK_BUNDLE_ID = org.freedesktop.Platform.VulkanLayer.krosshair
FLATPAK_VERSIONS = 24.08 25.08 26.08
FLATPAK_LIB_PATH = /usr/lib/extensions/vulkan/krosshair/lib/krosshair.so
KROSSHAIR_STD_PATH = /usr/lib/krosshair/krosshair.so
FLATPAK_MANIST_FILE = flatpak/$(FLATPAK_BUNDLE_ID).yml
FLATPAK_METAINFO_FILE = flatpak/$(FLATPAK_BUNDLE_ID).metainfo.xml
PKGVERSION := $(shell grep -m1 '^pkgver=' PKGBUILD | cut -d= -f2)

# Detect if flathub is available at user level; fall back to system-level
FLATPAK_USER_REMOTE := $(shell flatpak --user remotes 2>/dev/null | grep -q flathub && echo 1)

ifeq ($(FLATPAK_USER_REMOTE),1)
  FLATPAK_INSTALL = flatpak install --user -y
else
  FLATPAK_INSTALL = sudo flatpak install -y
endif


.PHONY: all release test clean install flatpak-build flatpak-install

all:
	mkdir -p lib
	$(CC) $(CCFLAGS) $(SOURCES) $(LDFLAGS) -o $(LIBRARY)

release:
	$(MAKE) RELEASE=1 all

# Build every tests/test_*.c against all layer sources (with UNIT_TEST
# defined) and run each resulting binary. Fails on the first error.
test: all
	mkdir -p $(BUILD_DIR)
	@for t in $(TEST_SOURCES); do \
		name=$$(basename $$t .c); \
		echo "== building test $$name"; \
		$(CC) $(TEST_CCFLAGS) $$t $(SOURCES) -o $(BUILD_DIR)/$$name -lm -lpthread || exit 1; \
	done
	@for b in $(BUILD_DIR)/test_*; do \
		[ "$$b" = "$(BUILD_DIR)/test_mock_icd" ] && continue; \
		echo "== running $$b"; \
		$$b || exit 1; \
	done

	# angle 2: end-to-end through the real Vulkan loader, with the mock
	# ICD as the only driver and the layer enabled via KROSSHAIR=1.
	# The test layer manifest is generated with an absolute library_path
	# (the checked-in krosshair.json points at the install location).
	echo "== building mock ICD"
	@mkdir -p $(MOCK_ICD_DIR)
	# The loader dlopens library_path verbatim (no CWD/manifest-dir
	# resolution), so the ICD manifest must carry an absolute path.
	@printf '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n    "library_path": "%s",\n    "api_version": "1_3_0",\n    "name": "VK_ICD_MOCK"\n  }\n}\n' $(CURDIR)/$(MOCK_ICD_SO) > $(MOCK_ICD_DIR)/mock_icd.json
	$(CC) -Wall -std=c99 -ggdb -fPIC -shared tests/mock_icd.c -o $(MOCK_ICD_SO) || exit 1
	$(CC) $(TEST_CCFLAGS) tests/test_mock_icd.c -o $(BUILD_DIR)/test_mock_icd -lvulkan -ldl -lm -lpthread || exit 1
	@mkdir -p $(BUILD_DIR)/vk_layer_path
	@printf '{\n "file_format_version": "1.0.0",\n "layer": {\n  "name": "VK_LAYER_KROSSHAIR_overlay",\n  "type": "GLOBAL",\n  "api_version": "1.3.0",\n  "library_path": "%s/lib/krosshair.so",\n  "implementation_version": "1",\n  "description": "Crosshair Overlay (test manifest)",\n  "functions": {\n   "vkGetInstanceProcAddr": "overlay_GetInstanceProcAddr",\n   "vkGetDeviceProcAddr": "overlay_GetDeviceProcAddr"\n  },\n  "enable_environment": { "KROSSHAIR": "1" },\n  "disable_environment": { "DISABLE_KROSSHAIR": "1" }\n }\n}\n' $(CURDIR) > $(BUILD_DIR)/vk_layer_path/krosshair.json
	echo "== running $(BUILD_DIR)/test_mock_icd (layer over mock ICD)"
	# Some packaged loaders (Debian 1.4.309, Arch 1.4.357) silently skip
	# layers found via VK_LAYER_PATH, so the manifest is also placed in
	# the standard per-user implicit layer dir and removed afterwards.
	@mkdir -p "$(HOME)/.config/vulkan/implicit_layer.d" && cp $(BUILD_DIR)/vk_layer_path/krosshair.json "$(HOME)/.config/vulkan/implicit_layer.d/" && { VK_LAYER_PATH=$(BUILD_DIR)/vk_layer_path VK_DRIVER_FILES=$(MOCK_ICD_DIR)/mock_icd.json KROSSHAIR=1 MOCK_ICD_SO=$(CURDIR)/$(MOCK_ICD_SO) $(BUILD_DIR)/test_mock_icd; rc=$$?; rm -f "$(HOME)/.config/vulkan/implicit_layer.d/krosshair.json"; exit $$rc; }
	echo "== running $(BUILD_DIR)/test_mock_icd (custom APNG crosshair)"
	@mkdir -p "$(HOME)/.config/vulkan/implicit_layer.d" && cp $(BUILD_DIR)/vk_layer_path/krosshair.json "$(HOME)/.config/vulkan/implicit_layer.d/" && { VK_LAYER_PATH=$(BUILD_DIR)/vk_layer_path VK_DRIVER_FILES=$(MOCK_ICD_DIR)/mock_icd.json KROSSHAIR=1 KROSSHAIR_E2E_CUSTOM_IMG=1 MOCK_ICD_SO=$(CURDIR)/$(MOCK_ICD_SO) $(BUILD_DIR)/test_mock_icd; rc=$$?; rm -f "$(HOME)/.config/vulkan/implicit_layer.d/krosshair.json"; exit $$rc; }

install:
	sudo mkdir -p /usr/lib/krosshair
	sudo cp $(LIBRARY) /usr/lib/krosshair/krosshair.so
	sudo cp krosshair.json /usr/share/vulkan/implicit_layer.d/krosshair.json

clean:
	rm -f $(LIBRARY)
	rm -rf $(BUILD_DIR)

# Builds the flatpak into $(BUILD_DIR) (does NOT install it on the host).
flatpak-build:
	mkdir -p $(FLATPAK_BUILD_DIR)

	# Check and abort if the manifest, metainfo, or krosshair.json contain uncommitted changes. This is necessary as the build will temporarily modify the files during the build.
	git diff --exit-code $(FLATPAK_MANIST_FILE) $(FLATPAK_METAINFO_FILE) krosshair.json || { echo "ERROR: $(FLATPAK_MANIST_FILE), $(FLATPAK_METAINFO_FILE), or krosshair.json has uncommitted changes"; exit 1; }

	# Modify the metainfo file and krosshair.json for the flatpak build.
	sed -i "s|<release version=\"[^\"]*\" date=\"[^\"]*\"/>|<release version=\"$(PKGVERSION)\" date=\"$$(date +%Y-%m-%d)\"/>|" $(FLATPAK_METAINFO_FILE) && \
	sed -i "s|$(KROSSHAIR_STD_PATH)|$(FLATPAK_LIB_PATH)|" krosshair.json && \
	for VER in $(FLATPAK_VERSIONS); do \
		echo -e "\n-----\nBuilding flatpak for version \"$$VER\"...\n-----\n"; \
		mkdir -p $(FLATPAK_BUILD_DIR_INTERMEDIATE)/$$VER || exit 1; \
		sed -i "s|^branch:.*|branch: \"$$VER\"|" $(FLATPAK_MANIST_FILE) || exit 1; \
		sed -i "s|^runtime-version:.*|runtime-version: \"$$VER\"|" $(FLATPAK_MANIST_FILE) || exit 1; \
		# flatpak SDK install \
		echo -e "\n-----\nRunning flatpak install for \"org.freedesktop.Sdk//$$VER\"...\n-----\n"; \
		$(FLATPAK_INSTALL) flathub org.freedesktop.Sdk//$$VER org.freedesktop.Platform//$$VER || exit 1; \
		# flatpak-builder \
		echo -e "\n-----\nRunning flatpak-builder for \"$$VER\"...\n-----\n"; \
		flatpak-builder --force-clean $(FLATPAK_BUILD_DIR_INTERMEDIATE)/$$VER $(FLATPAK_MANIST_FILE) || exit 1; \
		# flatpak build-export \
		echo -e "\n-----\nRunning flatpak build-export for \"$$VER\"...\n-----\n"; \
		flatpak build-export $(FLATPAK_EXPORT_DIR) $(FLATPAK_BUILD_DIR_INTERMEDIATE)/$$VER $$VER || exit 1; \
		# flatpak build-bundle \
		echo -e "\n-----\nRunning flatpak build-bundle for \"$$VER\"...\n-----\n"; \
		flatpak build-bundle --runtime $(FLATPAK_EXPORT_DIR) $(FLATPAK_BUILD_DIR)/$(FLATPAK_BUNDLE_ID)_$$VER.flatpak $(FLATPAK_BUNDLE_ID) $$VER || exit 1; \
	done && \
	git checkout -- $(FLATPAK_MANIST_FILE) $(FLATPAK_METAINFO_FILE) krosshair.json

# Installs the flatpak (detects user vs system level automatically)
flatpak-install: flatpak-build
	for VER in $(FLATPAK_VERSIONS); do \
		$(FLATPAK_INSTALL) --reinstall $(FLATPAK_BUILD_DIR)/$(FLATPAK_BUNDLE_ID)_$$VER.flatpak || exit 1; \
	done
	echo -e "\n-----\nInstalled flatpak packages\n-----\n"
	flatpak list  | grep krosshair

# Runs inside the flatpak-builder sandbox (invoked from the .yml).
# Builds the layer and stages files into the flatpak output (/app).
flatpak-builder-callback: release
	install -Dm755 $(LIBRARY) -t ${FLATPAK_DEST}/lib/
	install -Dm644 krosshair.json -t ${FLATPAK_DEST}/share/vulkan/implicit_layer.d/
	grep -H library_path ${FLATPAK_DEST}/share/vulkan/implicit_layer.d/krosshair.json
