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

.PHONY: all build ci-build clean upload monitor preflight check-secrets \
        info help

all: build

build:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN)

ci-build: build

clean:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN) -t clean

upload:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO_RUN) -t upload

monitor:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO) -d "$(PROJECT_PATH)" device monitor

preflight:
	@sh "$(PROJECT_PATH)/tools/preflight-git-push.sh"

check-secrets: preflight

info:
	@set -eu; \
	command -v "$(PIO)" >/dev/null 2>&1 || { echo "ERROR: PlatformIO CLI '$(PIO)' tidak ditemukan."; exit 127; }; \
	$(PIO) -d "$(PROJECT_PATH)" system info

help:
	@echo "FieldRadio ESP32-S3 build targets"
	@echo ""
	@echo "  make build                 Build firmware locally"
	@echo "  make ci-build              Same build entry point used by CI"
	@echo "  make clean                 Clean PlatformIO build output"
	@echo "  make upload                Build and upload to the selected board"
	@echo "  make monitor               Open serial monitor"
	@echo "  make preflight             Scan Git state for credential/secret leakage"
	@echo "  make check-secrets         Alias for make preflight"
	@echo "  make info                  Show PlatformIO information"
	@echo ""
	@echo "Optional: make PIO_ENV=esp32-s3-wroom-1 build"
