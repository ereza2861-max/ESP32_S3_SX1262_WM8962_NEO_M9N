#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARTIFACTS="${PRODUCTION_ACCEPTANCE_DIR:-$ROOT/artifacts/production}"
BUILD="$ARTIFACTS/build.bin"
HASH="$ARTIFACTS/build.sha256"
EFUSE="$ARTIFACTS/efuse-summary.txt"
CERT="$ARTIFACTS/device-cert.pem"

for path in "$BUILD" "$HASH" "$EFUSE" "$CERT"; do
  if [ ! -s "$path" ]; then
    echo "ERROR: required production acceptance artifact is missing or empty: $path" >&2
    exit 1
  fi
done

# build.sha256 may contain either a normal sha256sum line or a path-qualified
# line; verify against the exact artifact under test.
(
  cd "$ARTIFACTS"
  sha256sum -c "$(basename "$HASH")"
)

# Accept the common espefuse summary spellings, but fail closed if the summary
# does not explicitly record both required production security features.
if ! grep -Eiq 'SECURE_BOOT(_V2)?[^[:alnum:]]*(ENABLED|true|1)|SECURE_BOOT_EN[^[:alnum:]]*(ENABLED|true|1)' "$EFUSE"; then
  echo "ERROR: Secure Boot is not explicitly enabled in efuse-summary.txt" >&2
  exit 1
fi
if ! grep -Eiq 'FLASH_ENCRYPT(ION|_EN)?[^[:alnum:]]*(ENABLED|true|1)|FLASH_CRYPT_CNT[^[:alnum:]]*(0x)?[1-9A-Fa-f]' "$EFUSE"; then
  echo "ERROR: Flash Encryption is not explicitly enabled in efuse-summary.txt" >&2
  exit 1
fi

# A certificate is an acceptance artifact only when the PEM has content. The
# certificate parser belongs in the manufacturing/HIL evidence pipeline.
if ! grep -q -- '-----BEGIN CERTIFICATE-----' "$CERT" ||
   ! grep -q -- '-----END CERTIFICATE-----' "$CERT"; then
  echo "ERROR: device-cert.pem is not a PEM certificate" >&2
  exit 1
fi

echo "OK: build SHA256, eFuse security state, and device certificate checks passed."
