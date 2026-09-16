# FieldRadio build entry point.
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

.PHONY: all build build-log ci-build clean upload monitor provision check-provisioning \
        preflight check-secrets download-artifacts download-build-log download-ci auth-help info help

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

build-log: check-provisioning
	@set -o pipefail; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN) 2>&1 | tee "$(PROJECT_PATH)/build.log"

download-artifacts:
	@$(gh_check); \
	run_id="$(call gh_run_id)"; \
	test -n "$$run_id" || { echo "ERROR: Tidak ditemukan CI run untuk commit $(CI_COMMIT)."; exit 1; }; \
	mkdir -p "$(ARTIFACT_DIR)"; \
	$(GH) run download "$$run_id" --repo "$$( $(GH) repo view --json nameWithOwner --jq .nameWithOwner )" \
		--name "esp32-s3-firmware-$(CI_COMMIT)" --dir "$(ARTIFACT_DIR)"

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
		echo "WARN: firmware artifact tidak tersedia (misalnya karena compile gagal). Compile log tetap tersedia di $(ARTIFACT_DIR)."; \
	fi

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
	@sh "$(PROJECT_PATH)/tools/preflight-git-push.sh"

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
	@echo "  make download-artifacts    Download firmware artifacts from CI for HEAD"
	@echo "  make download-build-log    Download the CI compile log for HEAD"
	@echo "  make download-ci           Download compile log, then firmware artifacts (best effort on failed builds)"
	@echo "  make download-build-log CI_RUN_ID=<id>  Download log from a specific Actions run"
	@echo "  make download-artifacts CI_RUN_ID=<id> Download firmware from a specific Actions run"
	@echo "  make ci-build              Same build entry point used by CI"
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
	@echo "Production security sdkconfig prepared."
	@echo "Review sdkconfig and docs/SECURITY_PROVISIONING.md before flashing."
