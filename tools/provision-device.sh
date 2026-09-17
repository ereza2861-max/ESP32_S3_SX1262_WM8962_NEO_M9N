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

  sta_ssid="${FIELDRADIO_STA_SSID:-}"
  sta_password="${FIELDRADIO_STA_PASSWORD:-}"
  mqtt_host="${FIELDRADIO_MQTT_HOST:-broker.emqx.io}"
  mqtt_port="${FIELDRADIO_MQTT_PORT:-8883}"
  mqtt_user="${FIELDRADIO_MQTT_USERNAME:-}"
  mqtt_password="${FIELDRADIO_MQTT_PASSWORD:-}"

  lora_key="${FIELDRADIO_LORA_KEY_HEX:-}"
  if [ -z "$lora_key" ] && [ -t 0 ]; then
    printf '%s' "LoRa AES-128 key (32 hex chars, blank=generate): "
    IFS= read -r lora_key
  fi
  if [ -z "$lora_key" ]; then
    lora_key="$(openssl rand -hex 16)"
  fi

  python3 - "$ap_ssid" "$ap_password" "$web_user" "$web_password" "$lora_key" "$sta_ssid" "$sta_password" "$mqtt_host" "$mqtt_port" "$mqtt_user" "$mqtt_password" <<'PY'
import re
import sys
ssid, appass, webuser, webpass, key, sta_ssid, sta_pass, mqtt_host, mqtt_port, mqtt_user, mqtt_pass = sys.argv[1:]

def byte_len(value):
    return len(value.encode("utf-8"))

for name, value, minimum, maximum in (
    ("AP SSID", ssid, 1, 32),
    ("AP password", appass, 8, 63),
    ("WebUI username", webuser, 1, 32),
    ("WebUI password", webpass, 8, 63),
):
    if not minimum <= byte_len(value) <= maximum:
        raise SystemExit(f"ERROR: {name} must be {minimum}..{maximum} UTF-8 bytes.")
    if any(ord(c) < 0x20 or ord(c) == 0x7f for c in value):
        raise SystemExit(f"ERROR: {name} contains a control character.")
    if any(c in value for c in '\\"'):
        raise SystemExit(f"ERROR: {name} may not contain backslash or double-quote characters when stored in LocalConfig.h.")

if appass == webpass:
    raise SystemExit("ERROR: AP and WebUI passwords must be different.")
if sta_ssid and not 1 <= byte_len(sta_ssid) <= 32:
    raise SystemExit("ERROR: STA SSID must be 1..32 UTF-8 bytes when supplied.")
if sta_pass and not 8 <= byte_len(sta_pass) <= 63:
    raise SystemExit("ERROR: STA password must be 8..63 UTF-8 bytes when supplied.")
if not 1 <= byte_len(mqtt_host) <= 253:
    raise SystemExit("ERROR: MQTT host must be 1..253 UTF-8 bytes.")
if not mqtt_port.isdigit() or not 1 <= int(mqtt_port) <= 65535:
    raise SystemExit("ERROR: MQTT port must be 1..65535.")
if len(mqtt_user) > 128 or len(mqtt_pass) > 128:
    raise SystemExit("ERROR: MQTT credentials must be <=128 characters.")
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
#define FIELDRADIO_STA_SSID "$sta_ssid"
#define FIELDRADIO_STA_PASSWORD "$sta_password"
#define FIELDRADIO_DEVICE_ID "${FIELDRADIO_DEVICE_ID:-ESP32S3_VOICE_NODE_01}"
#define FIELDRADIO_CALLSIGN "${FIELDRADIO_CALLSIGN:-FIELD_RADIO_01}"
#define FIELDRADIO_MQTT_HOST "$mqtt_host"
#define FIELDRADIO_MQTT_PORT $mqtt_port
#define FIELDRADIO_MQTT_USERNAME "$mqtt_user"
#define FIELDRADIO_MQTT_PASSWORD "$mqtt_password"
#define FIELDRADIO_MQTT_TOPIC_ROOT "${FIELDRADIO_MQTT_TOPIC_ROOT:-fieldradio}"
#define FIELDRADIO_MQTT_SERVER_NAME "${FIELDRADIO_MQTT_SERVER_NAME:-broker.emqx.io}"
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

python3 - "$TLS_DNS" "$TLS_IP" "$TLS_DAYS" <<'PY'
import ipaddress
import re
import sys

dns, ip, days = sys.argv[1:]
if not 1 <= len(dns.encode("utf-8")) <= 253 or not re.fullmatch(
    r"(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\.(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*",
    dns,
):
    raise SystemExit("ERROR: FIELDRADIO_TLS_DNS is not a valid DNS name.")
try:
    ipaddress.ip_address(ip)
except ValueError:
    raise SystemExit("ERROR: FIELDRADIO_TLS_IP is not a valid IP address.")
if not days.isdigit() or not 1 <= int(days) <= 825:
    raise SystemExit("ERROR: FIELDRADIO_TLS_DAYS must be an integer from 1 to 825.")
PY

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
