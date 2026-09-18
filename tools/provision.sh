#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SECRETS="$ROOT/secrets"
ARTIFACTS="$ROOT/artifacts/production"
PIO_ENV="${FIELDRADIO_PIO_ENV:-esp32-s3-wroom-1}"
PORT="${FIELDRADIO_PORT:-}"
SB_BLOCK="${FIELDRADIO_SECURE_BOOT_BLOCK:-BLOCK_KEY0}"
FE_BLOCK="${FIELDRADIO_FLASH_ENCRYPTION_BLOCK:-BLOCK_KEY1}"
CONFIRM=""
BURN=0
STATUS_ONLY=0
NO_FLASH=0

SB_KEY="$SECRETS/secure_boot_signing_key.pem"
FE_KEY="$SECRETS/flash_encryption_key.bin"
SDKCONFIG="$ROOT/sdkconfig"
SDKCONFIG_BACKUP=""
BOOTLOADER="$ROOT/.pio/build/$PIO_ENV/bootloader.bin"
PARTITIONS="$ROOT/.pio/build/$PIO_ENV/partitions.bin"
APP="$ROOT/.pio/build/$PIO_ENV/firmware.bin"
SIGNED_BOOTLOADER="$ARTIFACTS/bootloader.signed.bin"
SIGNED_APP="$ARTIFACTS/firmware.signed.bin"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf 'INFO: %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }

usage() {
    cat <<'EOF'
Usage:
  ./tools/provision.sh [--port PORT] [--burn] [--no-flash]
  ./tools/provision.sh --status --port PORT
  ./tools/provision.sh --help

Default:
  inspect eFuse -> ensure tools -> ensure secrets -> build release artifacts ->
  sign -> verify -> flash only when the device is already provisioned for the
  selected production workflow.

--burn:
  explicitly enables the irreversible manufacturing step. It requires the
  exact confirmation token BURN-IRREVERSIBLE. It never accepts y/yes.

--no-flash:
  build/sign/verify only.

--status:
  inspect chip/eFuse state and flash ID only.

Environment overrides:
  FIELDRADIO_PORT
  FIELDRADIO_PIO_ENV (default: esp32-s3-wroom-1)
  FIELDRADIO_SECURE_BOOT_BLOCK (default: BLOCK_KEY0)
  FIELDRADIO_FLASH_ENCRYPTION_BLOCK (default: BLOCK_KEY1)
EOF
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"
}

find_cmd() {
    local modern="$1" legacy="$2"
    if command -v "$modern" >/dev/null 2>&1; then
        printf '%s' "$modern"
    elif command -v "$legacy" >/dev/null 2>&1; then
        printf '%s' "$legacy"
    else
        die "required tool not found: $modern or $legacy"
    fi
}

cleanup() {
    if [[ -n "$SDKCONFIG_BACKUP" && -f "$SDKCONFIG_BACKUP" ]]; then
        mv -f -- "$SDKCONFIG_BACKUP" "$SDKCONFIG"
    fi
}
trap cleanup EXIT HUP INT TERM

while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --port)
            (($# >= 2)) || die "--port requires a value"
            PORT="$2"; shift 2 ;;
        --burn) BURN=1; shift ;;
        --status) STATUS_ONLY=1; shift ;;
        --no-flash) NO_FLASH=1; shift ;;
        *) die "unknown argument: $1 (use --help)" ;;
    esac
done

[[ -d "$ROOT/.git" || -f "$ROOT/.git/config" ]] || die "repository root is not a Git worktree: $ROOT"
[[ -f "$ROOT/platformio.ini" ]] || die "platformio.ini not found"
[[ -f "$ROOT/sdkconfig.secure.defaults" ]] || die "sdkconfig.secure.defaults not found"
[[ -f "$ROOT/no_ota.csv" ]] || die "no_ota.csv not found"
[[ "$PIO_ENV" == "esp32-s3-wroom-1" ]] || die "production provisioning is restricted to the ESP32-S3 environment esp32-s3-wroom-1; refusing PIO_ENV=$PIO_ENV"

ESPEFUSE="$(find_cmd espefuse espefuse.py)"
ESPTOOL="$(find_cmd esptool esptool.py)"
ESPSECURE="$(find_cmd espsecure espsecure.py)"
need_cmd python3
need_cmd openssl
need_cmd pio

[[ -n "$PORT" ]] || die "serial port is required; pass --port PORT or set FIELDRADIO_PORT"

mkdir -p -- "$SECRETS" "$ARTIFACTS"
chmod 700 "$SECRETS" "$ARTIFACTS"

efuse_summary() {
    "$ESPEFUSE" --port "$PORT" --chip esp32s3 summary
}

status() {
    info "Target chip/eFuse status:"
    efuse_summary
    info "SPI flash identification:"
    "$ESPTOOL" --port "$PORT" flash-id
}

if ((STATUS_ONLY)); then
    status
    exit 0
fi

if [[ ! -f "$SB_KEY" ]]; then
    info "Secure Boot V2 signing key is absent; generating a new RSA-3072 key."
    umask 077
    "$ESPSECURE" generate-signing-key --version 2 --scheme rsa3072 "$SB_KEY"
    chmod 600 "$SB_KEY"
else
    info "Keeping existing Secure Boot signing key; no overwrite performed."
fi

[[ -f "$SB_KEY" ]] || die "Secure Boot signing key was not created"
chmod 600 "$SB_KEY"

if [[ ! -f "$FE_KEY" ]]; then
    info "Per-device Flash Encryption key is absent; generating a fresh 256-bit key."
    umask 077
    python3 - "$FE_KEY" <<'PY'
from pathlib import Path
import secrets, sys
p = Path(sys.argv[1])
p.write_bytes(secrets.token_bytes(32))
PY
    chmod 600 "$FE_KEY"
else
    info "Keeping existing per-device Flash Encryption key; no overwrite performed."
fi

[[ "$(wc -c < "$FE_KEY")" -eq 32 ]] || die "Flash Encryption key must be exactly 32 bytes for XTS_AES_128_KEY"

info "Pre-burn eFuse summary (no eFuse write is performed by this step)."
status

# Production build is externally signed. The signing key is deliberately not
# injected into a tracked file or CI configuration.
if [[ -e "$SDKCONFIG" ]]; then
    SDKCONFIG_BACKUP="$(mktemp "$ROOT/sdkconfig.phase5.XXXXXX")"
    chmod 600 "$SDKCONFIG_BACKUP"
    cp -f -- "$SDKCONFIG" "$SDKCONFIG_BACKUP"
fi
cp -f -- "$ROOT/sdkconfig.secure.defaults" "$SDKCONFIG"
cat >>"$SDKCONFIG" <<EOF

# FASE 5 manufacturing overrides. Private key stays outside tracked files.
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y
EOF

info "Building PlatformIO environment: $PIO_ENV"
pio -d "$ROOT" run -e "$PIO_ENV"

[[ -f "$BOOTLOADER" ]] || die "built bootloader not found: $BOOTLOADER"
[[ -f "$PARTITIONS" ]] || die "built partition table not found: $PARTITIONS"
[[ -f "$APP" ]] || die "built application not found: $APP"

info "Signing bootloader and application with Secure Boot V2."
"$ESPSECURE" sign-data --version 2 --keyfile "$SB_KEY" \
    --output "$SIGNED_BOOTLOADER" "$BOOTLOADER"
"$ESPSECURE" sign-data --version 2 --keyfile "$SB_KEY" \
    --output "$SIGNED_APP" "$APP"

info "Verifying Secure Boot V2 signatures."
"$ESPSECURE" verify-signature --version 2 --keyfile "$SB_KEY" "$SIGNED_BOOTLOADER"
"$ESPSECURE" verify-signature --version 2 --keyfile "$SB_KEY" "$SIGNED_APP"

chmod 600 "$SIGNED_BOOTLOADER" "$SIGNED_APP"
info "Signed artifacts are ready under artifacts/production/."

if ((NO_FLASH)); then
    info "--no-flash selected; stopping before hardware flash/eFuse operations."
    exit 0
fi

# A fresh device must not receive a production plaintext image through a
# development workflow. For a fresh unit, --burn is the only accepted hardware
# path.
if (( ! BURN )); then
    die "fresh/unknown eFuse state requires explicit --burn; refusing automatic production eFuse or plaintext flash provisioning"
fi

printf '\n'
printf '%s\n' 'WARNING: EFUSE BURN IS IRREVERSIBLE.'
printf '%s\n' "Target: ESP32-S3"
printf '%s\n' "Serial port: $PORT"
printf '%s\n' "Secure Boot digest block: $SB_BLOCK"
printf '%s\n' "Flash Encryption key block: $FE_BLOCK"
printf '%s\n' "Signing key path: $SB_KEY"
printf '%s\n' "Flash-encryption key path: $FE_KEY"
printf '%s\n' 'The key files are never printed.'
printf '%s\n\n' 'Review the eFuse summary above before continuing.'
printf '%s' 'Type exactly BURN-IRREVERSIBLE to continue: '
IFS= read -r CONFIRM
[[ "$CONFIRM" == "BURN-IRREVERSIBLE" ]] || die "confirmation did not match exactly; no eFuse was burned"

# Re-read immediately before the irreversible operation.
info "Final eFuse pre-check:"
status

info "Enrolling Secure Boot V2 public-key digest."
"$ESPEFUSE" --port "$PORT" --chip esp32s3 burn-key-digest \
    "$SB_BLOCK" "$SB_KEY" SECURE_BOOT_DIGEST0

info "Enrolling per-device Flash Encryption key."
"$ESPEFUSE" --port "$PORT" --chip esp32s3 burn-key \
    "$FE_BLOCK" "$FE_KEY" XTS_AES_128_KEY

info "Enabling Secure Boot V2 eFuse."
"$ESPEFUSE" --port "$PORT" --chip esp32s3 burn-efuse SECURE_BOOT_EN

# Release-mode Flash Encryption must not be treated as a development flash.
# Do not burn SPI_BOOT_CRYPT_CNT here: with plaintext flash still present that
# would make the device unable to boot. The production bootloader performs the
# first encrypted-boot transition and burns the remaining release-mode state.
info "Secure Boot is enrolled. Flash Encryption transition is delegated to the production Release-mode bootloader on first boot."
info "Do not power-cycle during the first encryption pass."

# On a fresh device, the official ESP-IDF Release-mode flow flashes the signed
# plaintext images once. The second-stage bootloader then encrypts them in place
# using the pre-burned per-device key and completes the Release-mode eFuse state.
# Do not pre-encrypt these first-boot images: doing so while SPI_BOOT_CRYPT_CNT is
# still unset would cause the bootloader to treat ciphertext as plaintext.
info "Flashing signed production images for the first Release-mode boot."
"$ESPTOOL" --port "$PORT" write-flash \
    0x0 "$SIGNED_BOOTLOADER" \
    0x8000 "$PARTITIONS" \
    0x10000 "$SIGNED_APP"

info "Waiting for the device to reboot and complete the Release-mode first boot."
sleep 5

warn "The Release-mode first boot may switch ROM download access to Secure Download Mode; espefuse may no longer be usable afterwards."
warn "Verify security state using the application/ROM security-information path and retain the pre-burn eFuse summary in the manufacturing record."
warn "Do not delete the per-device Flash Encryption key until the device has booted and the production images have been verified."
warn "After successful manufacturing verification, remove the local flash-encryption key according to the production key-retention policy."
warn "OTA is FUTURE/TODO in this repository; do not claim OTA production support yet."
