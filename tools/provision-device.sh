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
  est_server_url="${FIELDRADIO_EST_SERVER_URL:-}"
  est_label="${FIELDRADIO_EST_LABEL:-/.well-known/est}"
  est_auth_mode="${FIELDRADIO_EST_AUTH_MODE:-0}"
  est_username="${FIELDRADIO_EST_USERNAME:-}"
  est_password="${FIELDRADIO_EST_PASSWORD:-}"
  est_bootstrap_token="${FIELDRADIO_EST_BOOTSTRAP_TOKEN:-}"

  # ASSUMPTION: radio overrides are applied only when at least one LoRa radio
  # environment variable is supplied; missing fields retain the Config.h defaults.
  lora_override_enabled=0
  if [ -n "${FIELDRADIO_LORA_FREQ_MHZ:-}" ] ||
     [ -n "${FIELDRADIO_LORA_BW_KHZ:-}" ] ||
     [ -n "${FIELDRADIO_LORA_SF:-}" ] ||
     [ -n "${FIELDRADIO_LORA_CR:-}" ] ||
     [ -n "${FIELDRADIO_LORA_POWER_DBM:-}" ] ||
     [ -n "${FIELDRADIO_LORA_SYNC_WORD:-}" ]; then
    lora_override_enabled=1
    LORA_FREQ_MHZ=${FIELDRADIO_LORA_FREQ_MHZ:-923.0}
    LORA_BW_KHZ=${FIELDRADIO_LORA_BW_KHZ:-125.0}
    LORA_SF=${FIELDRADIO_LORA_SF:-7}
    LORA_CR=${FIELDRADIO_LORA_CR:-5}
    LORA_POWER_DBM=${FIELDRADIO_LORA_POWER_DBM:-14}
    LORA_SYNC_WORD=${FIELDRADIO_LORA_SYNC_WORD:-0x12}
  fi

  lorawan_requested=0
  if [ -n "${FIELDRADIO_LORAWAN_ENABLED:-}" ] ||
     [ -n "${FIELDRADIO_LORAWAN_MODE:-}" ] ||
     [ -n "${FIELDRADIO_LORAWAN_REGION:-}" ] ||
     [ -n "${FIELDRADIO_LORAWAN_JOIN_EUI:-}" ] ||
     [ -n "${FIELDRADIO_LORAWAN_APP_KEY:-}" ]; then
    lorawan_requested=1
    LORAWAN_ENABLED=${FIELDRADIO_LORAWAN_ENABLED:-0}
    LORAWAN_MODE=${FIELDRADIO_LORAWAN_MODE:-0}
    LORAWAN_REGION=${FIELDRADIO_LORAWAN_REGION:-1}
    LORAWAN_JOIN_EUI=${FIELDRADIO_LORAWAN_JOIN_EUI:-}
    LORAWAN_APP_KEY=${FIELDRADIO_LORAWAN_APP_KEY:-}
  fi

  lora_key="${FIELDRADIO_LORA_KEY_HEX:-}"
  if [ -z "$lora_key" ] && [ -t 0 ]; then
    printf '%s' "LoRa AES-128 key (32 hex chars, blank=generate): "
    IFS= read -r lora_key
  fi
  if [ -z "$lora_key" ]; then
    lora_key="$(openssl rand -hex 16)"
  fi

  python3 - "$ap_ssid" "$ap_password" "$web_user" "$web_password" "$lora_key" "$sta_ssid" "$sta_password" "$mqtt_host" "$mqtt_port" "$mqtt_user" "$mqtt_password" "$est_server_url" "$est_label" "$est_auth_mode" "$est_username" "$est_password" "$est_bootstrap_token" "$lora_override_enabled" "${LORA_FREQ_MHZ:-}" "${LORA_BW_KHZ:-}" "${LORA_SF:-}" "${LORA_CR:-}" "${LORA_POWER_DBM:-}" "${LORA_SYNC_WORD:-}" <<'PY'
import math
import re
import sys
ssid, appass, webuser, webpass, key, sta_ssid, sta_pass, mqtt_host, mqtt_port, mqtt_user, mqtt_pass, est_url, est_label, est_mode, est_user, est_pass, est_token, lora_enabled, lora_freq, lora_bw, lora_sf, lora_cr, lora_power, lora_sync = sys.argv[1:]

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
if len(est_url) > 253 or (est_url and not est_url.startswith("https://")):
    raise SystemExit("ERROR: EST server URL must be empty or https:// with <=253 characters.")
if not est_label.startswith("/") or len(est_label) > 95:
    raise SystemExit("ERROR: EST label must start with '/' and be <=95 characters.")
if est_mode not in ("0", "1", "2"):
    raise SystemExit("ERROR: EST auth mode must be 0, 1, or 2.")
for name, value in (("EST server URL", est_url), ("EST label", est_label),
                    ("EST username", est_user), ("EST password", est_pass),
                    ("EST bootstrap token", est_token)):
    if any(ord(c) < 0x20 or ord(c) == 0x7f for c in value):
        raise SystemExit(f"ERROR: {name} contains a control character.")
    if any(c in value for c in '\\"'):
        raise SystemExit(f"ERROR: {name} may not contain backslash or double-quote characters when stored in LocalConfig.h.")
if len(est_user) > 64 or len(est_pass) > 64 or len(est_token) > 128:
    raise SystemExit("ERROR: EST username/password/token exceeds the configured maximum.")
if est_mode == "1" and (not est_user or not est_pass):
    raise SystemExit("ERROR: EST mode 1 requires FIELDRADIO_EST_USERNAME and FIELDRADIO_EST_PASSWORD.")
if est_mode == "2" and not est_token:
    raise SystemExit("ERROR: EST mode 2 requires FIELDRADIO_EST_BOOTSTRAP_TOKEN.")
if not re.fullmatch(r"[0-9A-Fa-f]{32}", key):
    raise SystemExit("ERROR: LoRa key must be exactly 32 hexadecimal characters.")

if lora_enabled == "1":
    try:
        freq = float(lora_freq)
        bw = float(lora_bw)
        sf = int(lora_sf, 10)
        cr = int(lora_cr, 10)
        power = int(lora_power, 10)
        sync_word = int(lora_sync, 0)
    except ValueError:
        raise SystemExit("ERROR: invalid LoRa radio override value.")
    if not math.isfinite(freq) or not 920.0 <= freq <= 923.0:
        raise SystemExit("ERROR: LoRa frequency must be 920.0..923.0 MHz.")
    if not math.isfinite(bw) or not 7.8 <= bw <= 250.0:
        raise SystemExit("ERROR: LoRa bandwidth must be 7.8..250.0 kHz.")
    if not 5 <= sf <= 12:
        raise SystemExit("ERROR: LoRa spreading factor must be 5..12.")
    if not 5 <= cr <= 8:
        raise SystemExit("ERROR: LoRa coding rate must be 5..8.")
    if not 2 <= power <= 17:
        raise SystemExit("ERROR: LoRa power must be 2..17 dBm.")
    if not 0 <= sync_word <= 0xFF:
        raise SystemExit("ERROR: LoRa sync word must be 0x00..0xFF.")
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
#define FIELDRADIO_EST_SERVER_URL "$est_server_url"
#define FIELDRADIO_EST_LABEL "$est_label"
#define FIELDRADIO_EST_AUTH_MODE $est_auth_mode
#define FIELDRADIO_EST_USERNAME "$est_username"
#define FIELDRADIO_EST_PASSWORD "$est_password"
#define FIELDRADIO_EST_BOOTSTRAP_TOKEN "$est_bootstrap_token"
EOF
  if [ "$lora_override_enabled" = "1" ]; then
    cat >>"$tmp" <<LORA
#define FIELDRADIO_LORA_FREQ_MHZ  $LORA_FREQ_MHZ
#define FIELDRADIO_LORA_BW_KHZ    $LORA_BW_KHZ
#define FIELDRADIO_LORA_SF        $LORA_SF
#define FIELDRADIO_LORA_CR        $LORA_CR
#define FIELDRADIO_LORA_POWER_DBM $LORA_POWER_DBM
#define FIELDRADIO_LORA_SYNC_WORD $LORA_SYNC_WORD
LORA
  fi
  mv -f "$tmp" "$LOCAL_CONFIG"
  trap - EXIT HUP INT TERM
  echo "Created include/LocalConfig.h (ignored by Git)."
fi

if [ "${lorawan_requested:-0}" = "1" ]; then
  echo "LoRaWAN environment settings were supplied; they are not written to LocalConfig.h."
  echo "Configure LoRaWAN via the WebUI after boot."
fi

if [ -e "$SECRETS/web_tls_cert.der" ] || [ -e "$SECRETS/web_tls_key.der" ]; then
  if [ "$FORCE" = "1" ]; then
    rm -f "$SECRETS/web_tls_cert.der" "$SECRETS/web_tls_key.der"
  else
    echo "Keeping existing TLS DER material."
    sh "$ROOT/tools/check-provisioning.sh"
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
sh "$ROOT/tools/check-provisioning.sh"
echo "TLS provisioning complete. Private key remains only in ignored local files."
echo "Build with: make build"
echo "Flash with: make upload [UPLOAD_PORT=/dev/ttyUSB0]"
