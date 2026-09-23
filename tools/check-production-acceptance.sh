#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARTIFACTS="$ROOT/artifacts/production"
MANIFEST="$ARTIFACTS/manifest.sha256"

required="
build.sha256
signing.log
efuse-summary.txt
provisioning.log
hil-report.md
"

if [ ! -d "$ARTIFACTS" ]; then
  echo "ERROR: production evidence directory is missing: $ARTIFACTS" >&2
  exit 1
fi

for name in $required; do
  if [ ! -s "$ARTIFACTS/$name" ]; then
    echo "ERROR: required production evidence is missing or empty: $name" >&2
    exit 1
  fi
done

if [ ! -s "$MANIFEST" ]; then
  echo "ERROR: production evidence hash manifest is missing: $MANIFEST" >&2
  exit 1
fi

(
  cd "$ARTIFACTS"
  sha256sum -c "$(basename "$MANIFEST")"
)

echo "OK: required production evidence exists and manifest hashes verify."
echo "NOTE: this script verifies supplied evidence files; it does not create, sign, provision, burn eFuses, or claim HIL validation."
