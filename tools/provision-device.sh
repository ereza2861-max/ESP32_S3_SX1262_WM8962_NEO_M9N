#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SECRETS="$ROOT/secrets"
LOCAL_CONFIG="$ROOT/include/LocalConfig.h"
FORCE="${FORCE:-0}"
TLS_DNS="${FIELDRADIO_TLS_DNS:-fieldradio.local}"
TLS_IP="${FIELDRADIO_TLS_IP:-192.168.4.1}"
TLS_DAYS="${FIELDRADIO_TLS_DAYS:-825}"

die() { echo "ERROR: $*" >&2; exit 1; }

read_secret() {
  if [ -t 0 ]; then
    trap 'stty echo 2>/dev/null || true; exit 130' INT TERM HUP
    stty -echo
    IFS= read -r REPLY
    status=$?
    stty echo
    trap - INT TERM HUP
    return "$status"
  fi
  IFS= read -r REPLY
}

command -v openssl >/dev/null 2>&1 || die "OpenSSL is required."
command -v python3 >/dev/null 2>&1 || die "Python 3 is required."
mkdir -p "$SECRETS"
umask 077

if [ "$FORCE" != "1" ] && [ -e "$LOCAL_CONFIG" ]; then
  echo "Keeping existing include/LocalConfig.h."
else
  ap_ssid="${FIELDRADIO_AP_SSID:-}"
  if [ -z "$ap_ssid" ]; then
    printf '%s' "AP SSID [FieldRadio]: "
    IFS= read -r ap_ssid
    ap_ssid=${ap_ssid:-FieldRadio}
  fi

  ap_password="${FIELDRADIO_AP_PASSWORD:-}"
  if [ -z "$ap_password" ]; then
    printf '%s' "AP password (8-63 chars): "
    read_secret
    ap_password="$REPLY"
    printf '\n'
  fi

  web_user="${FIELDRADIO_WEB_USER:-}"
  if [ -z "$web_user" ]; then
    printf '%s' "WebUI username [admin]: "
    IFS= read -r web_user
    web_user=${web_user:-admin}
  fi

  web_password="${FIELDRADIO_WEB_PASSWORD:-}"
  if [ -z "$web_password" ]; then
    printf '%s' "WebUI password (8-63 chars): "
    read_secret
    web_password="$REPLY"
    printf '\n'
  fi

  lora_key="${FIELDRADIO_LORA_KEY_HEX:-}"
  if [ -z "$lora_key" ] && [ -t 0 ]; then
    printf '%s' "LoRa AES-128 key (32 hex chars, blank=generate): "
    IFS= read -r lora_key
  fi
  if [ -z "$lora_key" ]; then
    lora_key="$(openssl rand -hex 16)"
  fi

  python3 - "$ap_ssid" "$ap_password" "$web_user" "$web_password" "$lora_key" <<'PY'
import re
import sys
ssid, appass, webuser, webpass, key = sys.argv[1:]
if not ssid or len(ssid) > 32:
    raise SystemExit("ERROR: AP SSID must be 1..32 characters.")
if not 8 <= len(appass) <= 63:
    raise SystemExit("ERROR: AP password must be 8..63 characters.")
if not webuser or len(webuser) > 32:
    raise SystemExit("ERROR: WebUI username must be 1..32 characters.")
if not 8 <= len(webpass) <= 63:
    raise SystemExit("ERROR: WebUI password must be 8..63 characters.")
if appass == webpass:
    raise SystemExit("ERROR: AP and WebUI passwords must be different.")
if any(c in value for value in (ssid, appass, webuser, webpass) for c in '\\\"'):
    raise SystemExit("ERROR: credentials may not contain backslash or double-quote characters when stored in LocalConfig.h.")
if not re.fullmatch(r"[0-9A-Fa-f]{32}", key):
    raise SystemExit("ERROR: LoRa key must be exactly 32 hexadecimal characters.")
PY

  tmp="$(mktemp)"
  trap 'rm -f "$tmp"' EXIT HUP INT TERM
  chmod 600 "$tmp"
  cat >"$tmp" <<EOF
#pragma once

// Device-local credentials. This file is ignored by Git.
#define FIELDRADIO_AP_SSID "$ap_ssid"
#define FIELDRADIO_AP_PASSWORD "$ap_password"
#define FIELDRADIO_WEB_USER "$web_user"
#define FIELDRADIO_WEB_PASSWORD "$web_password"
#define FIELDRADIO_LORA_KEY_HEX "$lora_key"
EOF
  mv -f "$tmp" "$LOCAL_CONFIG"
  trap - EXIT HUP INT TERM
  echo "Created include/LocalConfig.h (ignored by Git)."
fi

if [ -e "$SECRETS/web_tls_cert.der" ] || [ -e "$SECRETS/web_tls_key.der" ]; then
  if [ "$FORCE" = "1" ]; then
    rm -f "$SECRETS/web_tls_cert.der" "$SECRETS/web_tls_key.der"
  else
    echo "Keeping existing TLS DER material."
    "$ROOT/tools/check-provisioning.sh"
    exit 0
  fi
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$TLS_DAYS" \
  -keyout "$tmpdir/web_tls_key.pem" \
  -out "$tmpdir/web_tls_cert.pem" \
  -subj "/CN=$TLS_DNS" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth" \
  -addext "subjectAltName=DNS:$TLS_DNS,IP:$TLS_IP" >/dev/null 2>&1

openssl x509 -in "$tmpdir/web_tls_cert.pem" -outform DER \
  -out "$SECRETS/web_tls_cert.der"
openssl pkey -in "$tmpdir/web_tls_key.pem" -outform DER \
  -out "$SECRETS/web_tls_key.der"

chmod 600 "$SECRETS/web_tls_cert.der" "$SECRETS/web_tls_key.der"
"$ROOT/tools/check-provisioning.sh"
echo "TLS provisioning complete. Private key remains only in ignored local files."
echo "Build with: make build"
echo "Flash with: make upload [UPLOAD_PORT=/dev/ttyUSB0]"
