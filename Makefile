# FieldRadio build entry point.
# Use POSIX /bin/sh so Make targets also work on minimal Unix environments.
SHELL := /bin/sh
#
# Requirements:
#   - PlatformIO CLI (`pio`) on PATH
#   - Git only for the preflight target
#
# No git-apply or automatic-commit automation is performed here.

PROJECT_DIR ?= .
PIO ?= pio
PIO_ENV ?= esp32-s3-wroom-1
PIO_ARGS ?=

PROJECT_PATH := $(abspath $(PROJECT_DIR))
PIO_RUN := $(PIO) -d "$(PROJECT_PATH)" run -e "$(PIO_ENV)" $(PIO_ARGS)

.PHONY: all build build-log ci-build sensor-node-build clean upload monitor provision check-provisioning \
        preflight test-native check-secrets download-artifacts download-sensor-node-artifacts download-build-log download-ci \
        auth-help info help test test-hil fuzz failure-inject secure-boot-keys security-profile

GH ?= gh
CI_WORKFLOW ?= compile.yml
CI_COMMIT ?= $(shell git rev-parse HEAD 2>/dev/null || true)
# Optional explicit Actions run ID. Useful when the same commit has multiple CI runs.
CI_RUN_ID ?=
ARTIFACT_DIR ?= artifacts

gh_run_id = $(if $(strip $(CI_RUN_ID)),$(CI_RUN_ID),$$( $(GH) run list --workflow "$(CI_WORKFLOW)" --commit "$(CI_COMMIT)" --limit 1 --json databaseId --jq '.[0].databaseId' ))

gh_check = set -eu; \
	command -v "$(GH)" >/dev/null 2>&1 || { echo "ERROR: GitHub CLI '$(GH)' tidak ditemukan."; exit 127; }; \
	test -n "$(CI_COMMIT)" || { echo "ERROR: CI_COMMIT tidak dapat ditentukan."; exit 2; }


all: build

build: check-provisioning
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN)

ci-build:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN)

sensor-node-build:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO) -d "$(PROJECT_PATH)/sensor_node_esp32c3" run -e sensor_node_c3

build-log: check-provisioning
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	log="$(PROJECT_PATH)/build.log"; \
	set +e; \
	$(PIO_RUN) >"$$log" 2>&1; \
	pio_status=$$?; \
	cat "$$log"; \
	exit "$$pio_status"

download-artifacts:
	@$(gh_check); \
	run_id="$(call gh_run_id)"; \
	test -n "$$run_id" || { echo "ERROR: Tidak ditemukan CI run untuk commit $(CI_COMMIT)."; exit 1; }; \
	mkdir -p "$(ARTIFACT_DIR)"; \
	$(GH) run download "$$run_id" --repo "$$( $(GH) repo view --json nameWithOwner --jq .nameWithOwner )" \
		--name "esp32-s3-firmware-$(CI_COMMIT)" --dir "$(ARTIFACT_DIR)"

download-sensor-node-artifacts:
	@$(gh_check); \
	run_id="$(call gh_run_id)"; \
	test -n "$$run_id" || { echo "ERROR: Tidak ditemukan CI run untuk commit $(CI_COMMIT)."; exit 1; }; \
	mkdir -p "$(ARTIFACT_DIR)"; \
	$(GH) run download "$$run_id" --repo "$$( $(GH) repo view --json nameWithOwner --jq .nameWithOwner )" \
		--name "esp32-c3-sensor-node-$(CI_COMMIT)" --dir "$(ARTIFACT_DIR)"

download-build-log:
	@$(gh_check); \
	run_id="$(call gh_run_id)"; \
	test -n "$$run_id" || { echo "ERROR: Tidak ditemukan CI run untuk commit $(CI_COMMIT)."; exit 1; }; \
	mkdir -p "$(ARTIFACT_DIR)"; \
	$(GH) run download "$$run_id" --repo "$$( $(GH) repo view --json nameWithOwner --jq .nameWithOwner )" \
		--name "esp32-s3-build-log-$(CI_COMMIT)" --dir "$(ARTIFACT_DIR)"

download-ci:
	@set -eu; \
	$(MAKE) download-build-log CI_RUN_ID="$(CI_RUN_ID)" CI_COMMIT="$(CI_COMMIT)" CI_WORKFLOW="$(CI_WORKFLOW)" ARTIFACT_DIR="$(ARTIFACT_DIR)"; \
	if $(MAKE) download-artifacts CI_RUN_ID="$(CI_RUN_ID)" CI_COMMIT="$(CI_COMMIT)" CI_WORKFLOW="$(CI_WORKFLOW)" ARTIFACT_DIR="$(ARTIFACT_DIR)"; then \
		echo "CI firmware artifact berhasil diunduh."; \
	else \
		echo "WARN: gateway firmware artifact tidak tersedia (misalnya karena compile gagal)."; \
	fi; \
	if $(MAKE) download-sensor-node-artifacts CI_RUN_ID="$(CI_RUN_ID)" CI_COMMIT="$(CI_COMMIT)" CI_WORKFLOW="$(CI_WORKFLOW)" ARTIFACT_DIR="$(ARTIFACT_DIR)"; then \
		echo "CI sensor-node artifact berhasil diunduh."; \
	else \
		echo "WARN: sensor-node firmware artifact tidak tersedia."; \
	fi


test:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO) -d "$(PROJECT_PATH)" test -e native

test-hil:
	@echo "HIL is manual hardware validation; see test/hil/*.md"
	@echo "Host-only gates: make fuzz && make failure-inject"

fuzz:
	@set -eu; 	mkdir -p "$(PROJECT_PATH)/.host-test"; 	c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer 	  -I"$(PROJECT_PATH)/shared" "$(PROJECT_PATH)/shared/EncryptedFrameParser.h" 	  "$(PROJECT_PATH)/src/EncryptedFrameParser.cpp" "$(PROJECT_PATH)/test/test_fuzz_frame_parser.cpp" 	  -o "$(PROJECT_PATH)/.host-test/test_fuzz_frame_parser"; 	"$(PROJECT_PATH)/.host-test/test_fuzz_frame_parser"

failure-inject:
	@set -eu; 	mkdir -p "$(PROJECT_PATH)/.host-test"; 	c++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer 	  "$(PROJECT_PATH)/test/test_failure_injection.cpp" 	  -o "$(PROJECT_PATH)/.host-test/test_failure_injection"; 	"$(PROJECT_PATH)/.host-test/test_failure_injection"

secure-boot-keys:
	@set -eu; 	KEY_DIR="$${KEY_DIR:-$$HOME/.fieldradio/keys}"; 	mkdir -p "$$KEY_DIR"; chmod 700 "$$KEY_DIR"; 	command -v openssl >/dev/null 2>&1 || { echo "ERROR: openssl tidak ditemukan."; exit 127; }; 	test ! -e "$$KEY_DIR/secure_boot_signing_key.pem" || { echo "ERROR: signing key already exists; choose a new KEY_DIR."; exit 2; }; 	test ! -e "$$KEY_DIR/flash_encryption_key.bin" || { echo "ERROR: flash key already exists; choose a new KEY_DIR."; exit 2; }; 	openssl genrsa -out "$$KEY_DIR/secure_boot_signing_key.pem" 3072; 	openssl rand -out "$$KEY_DIR/flash_encryption_key.bin" 32; 	chmod 600 "$$KEY_DIR/secure_boot_signing_key.pem" "$$KEY_DIR/flash_encryption_key.bin"; 	echo "Key material generated in $$KEY_DIR; no eFuse or device operation was performed."

clean:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN) -t clean

upload: check-provisioning
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN) -t upload $(if $(UPLOAD_PORT),--upload-port "$(UPLOAD_PORT)",)

monitor:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO) -d "$(PROJECT_PATH)" device monitor

provision:
	@sh "$(PROJECT_PATH)/tools/provision-device.sh"

check-provisioning:
	@sh "$(PROJECT_PATH)/tools/check-provisioning.sh"

preflight:
	@python3 "$(PROJECT_PATH)/tools/check_partition_size.py" partitions.csv
	@python3 "$(PROJECT_PATH)/tools/check_gconfig_direct.py"
	@sh "$(PROJECT_PATH)/tools/preflight-git-push.sh"

test-native:
	@set -eu; \
	command -v c++ >/dev/null 2>&1 || { echo "ERROR: c++ compiler not found."; exit 127; }; \
	mkdir -p "$(PROJECT_PATH)/.host-test"; \
	c++ -std=c++17 -Wall -Wextra -Werror "$(PROJECT_PATH)/test/native/test_lora_config_lifecycle.cpp" \
		-o "$(PROJECT_PATH)/.host-test/test_lora_config_lifecycle"; \
	"$(PROJECT_PATH)/.host-test/test_lora_config_lifecycle"

check-secrets: preflight

auth-help:
	@echo "GitHub authentication: see docs/GITHUB_AUTH.md"
	@echo "Preferred local login: gh auth login (browser/SSH or credential manager)"
	@echo "Do not put PATs in source, build flags, or tracked files."

info:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO) -d "$(PROJECT_PATH)" system info

help:
	@echo "FieldRadio ESP32-S3 build targets"
	@echo ""
	@echo "  make build                 Build firmware locally"
	@echo "  make build-log             Build firmware and save output to build.log"
	@echo "  make download-artifacts    Download ESP32-S3 gateway firmware artifacts from CI for HEAD"
	@echo "  make download-sensor-node-artifacts  Download ESP32-C3 sensor-node artifacts"
	@echo "  make download-build-log    Download the CI compile log for HEAD"
	@echo "  make download-ci           Download compile log, then firmware artifacts (best effort on failed builds)"
	@echo "  make download-build-log CI_RUN_ID=<id>  Download log from a specific Actions run"
	@echo "  make download-artifacts CI_RUN_ID=<id> Download firmware from a specific Actions run"
	@echo "  make ci-build              Same gateway build entry point used by CI"
	@echo "  make sensor-node-build     Build the ESP32-C3 BLE sensor-node firmware"
	@echo "  make clean                 Clean PlatformIO build output"
	@echo "  make provision             Create local credentials and a device TLS certificate"
	@echo "  make check-provisioning     Validate local credentials and TLS material"
	@echo "  make upload                Build and upload to the selected board"
	@echo "  make monitor               Open serial monitor"
	@echo "  make preflight             Scan Git state for credential/secret leakage"
	@echo "  make check-secrets         Alias for make preflight"
	@echo "  make info                  Show PlatformIO information"
	@echo ""
	@echo "Optional: make PIO_ENV=esp32-s3-wroom-1 build"

.PHONY: security-profile
security-profile:
	@test -n "$(SECURE_BOOT_SIGNING_KEY)" || (echo "ERROR: set SECURE_BOOT_SIGNING_KEY=/secure/path/signing_key.pem"; exit 2)
	@test -f "$(SECURE_BOOT_SIGNING_KEY)" || (echo "ERROR: signing key not found"; exit 2)
	@cp -f sdkconfig.secure.defaults sdkconfig
	@printf '\\nCONFIG_SECURE_BOOT_SIGNING_KEY="%s"\\n' "$(SECURE_BOOT_SIGNING_KEY)" >> sdkconfig
	@printf '\\n# Production mode: Release flash encryption; secure UART download.\\nCONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y\\nCONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y\\n' >> sdkconfig
	@echo "Production security sdkconfig prepared for ESP32-S3."
	@echo "This target only prepares build configuration; it does not burn eFuses or flash hardware."
	@echo "Review docs/PRODUCTION.md and use tools/provision.sh for the manufacturing workflow."
