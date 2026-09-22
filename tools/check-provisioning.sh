#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SECRETS="$ROOT/secrets"
LOCAL_CONFIG="$ROOT/include/LocalConfig.h"
CERT="$SECRETS/web_tls_cert.der"
KEY="$SECRETS/web_tls_key.der"
EST_CA="$SECRETS/est_ca.pem"

fail() { echo "ERROR: $*" >&2; exit 1; }

command -v openssl >/dev/null 2>&1 || fail "OpenSSL is required for local HTTPS provisioning."
[ -f "$LOCAL_CONFIG" ] || fail "include/LocalConfig.h is missing; run 'make provision'."
[ -f "$CERT" ] || fail "secrets/web_tls_cert.der is missing; run 'make provision' or install a CA-issued certificate."
[ -f "$KEY" ] || fail "secrets/web_tls_key.der is missing; run 'make provision' or install a CA-issued key."

openssl x509 -inform DER -in "$CERT" -noout >/dev/null 2>&1 ||
  fail "TLS certificate is not valid DER X.509."
openssl pkey -inform DER -in "$KEY" -noout >/dev/null 2>&1 ||
  fail "TLS private key is not valid DER private-key material."

cert_pub="$(mktemp)"
key_pub="$(mktemp)"
trap 'rm -f "$cert_pub" "$key_pub"' EXIT HUP INT TERM

openssl x509 -inform DER -in "$CERT" -pubkey -noout >"$cert_pub" ||
  fail "Cannot extract the TLS certificate public key."
openssl pkey -inform DER -in "$KEY" -pubout >"$key_pub" ||
  fail "Cannot extract the TLS private-key public key."
cmp -s "$cert_pub" "$key_pub" ||
  fail "TLS certificate and private key do not match."

if [ -e "$EST_CA" ]; then
  openssl x509 -in "$EST_CA" -noout >/dev/null 2>&1 ||
    fail "secrets/est_ca.pem is not a valid PEM X.509 certificate."
fi

# Validate the local credential contract without printing any secret values.
python3 - "$LOCAL_CONFIG" <<'PY'
import math
import re
import sys
from pathlib import Path

path = sys.argv[1]
text = open(path, encoding="utf-8").read()

def macro(name):
    m = re.search(r'^\s*#define\s+' + re.escape(name) + r'\s+(.+?)\s*$', text, re.M)
    if not m:
        return None
    raw = m.group(1).strip()
    if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        return raw[1:-1]
    return raw

def numeric_macro(name, default):
    raw = macro(name)
    return default if raw is None else raw

ap_ssid = macro("FIELDRADIO_AP_SSID")
ap_password = macro("FIELDRADIO_AP_PASSWORD")
web_user = macro("FIELDRADIO_WEB_USER")
web_password = macro("FIELDRADIO_WEB_PASSWORD")
lora_key = macro("FIELDRADIO_LORA_KEY_HEX")

def byte_len(value):
    return len(value.encode("utf-8"))

for name, value, minimum, maximum in (
    ("FIELDRADIO_AP_SSID", ap_ssid, 1, 32),
    ("FIELDRADIO_AP_PASSWORD", ap_password, 8, 63),
    ("FIELDRADIO_WEB_USER", web_user, 1, 32),
    ("FIELDRADIO_WEB_PASSWORD", web_password, 8, 63),
):
    if not minimum <= byte_len(value) <= maximum:
        raise SystemExit(f"ERROR: {name} must be {minimum}..{maximum} UTF-8 bytes.")
    if any(ord(c) < 0x20 or ord(c) == 0x7f for c in value):
        raise SystemExit(f"ERROR: {name} contains a control character.")
    if any(c in value for c in '\\\"'):
        raise SystemExit(f"ERROR: {name} may not contain backslash or double-quote characters.")

if ap_password == web_password:
    raise SystemExit("ERROR: AP and WebUI passwords must be different.")
if not lora_key or not re.fullmatch(r"[0-9A-Fa-f]{32}", lora_key):
    raise SystemExit("ERROR: FIELDRADIO_LORA_KEY_HEX must contain exactly 32 hexadecimal characters.")

try:
    lora_freq = float(numeric_macro("FIELDRADIO_LORA_FREQ_MHZ", "923.0"))
    lora_bw = float(numeric_macro("FIELDRADIO_LORA_BW_KHZ", "125.0"))
    lora_sf = int(numeric_macro("FIELDRADIO_LORA_SF", "7"), 0)
    lora_cr = int(numeric_macro("FIELDRADIO_LORA_CR", "5"), 0)
    lora_power = int(numeric_macro("FIELDRADIO_LORA_POWER_DBM", "14"), 0)
except ValueError:
    raise SystemExit("ERROR: invalid LoRa radio configuration in LocalConfig.h.")
if not math.isfinite(lora_freq) or not 920.0 <= lora_freq <= 923.0:
    raise SystemExit("ERROR: FIELDRADIO_LORA_FREQ_MHZ must be 920.0..923.0 MHz.")
if not math.isfinite(lora_bw) or not 7.8 <= lora_bw <= 250.0:
    raise SystemExit("ERROR: FIELDRADIO_LORA_BW_KHZ must be 7.8..250.0 kHz.")
if not 5 <= lora_sf <= 12:
    raise SystemExit("ERROR: FIELDRADIO_LORA_SF must be 5..12.")
if not 5 <= lora_cr <= 8:
    raise SystemExit("ERROR: FIELDRADIO_LORA_CR must be 5..8.")
if not 2 <= lora_power <= 17:
    raise SystemExit("ERROR: FIELDRADIO_LORA_POWER_DBM must be 2..17 dBm.")

mqtt_port_raw = numeric_macro("FIELDRADIO_MQTT_PORT", "8883")
try:
    mqtt_port = int(mqtt_port_raw, 0)
except ValueError:
    raise SystemExit("ERROR: FIELDRADIO_MQTT_PORT is not a valid integer.")
if not 1 <= mqtt_port <= 65535:
    raise SystemExit("ERROR: FIELDRADIO_MQTT_PORT must be 1..65535.")
if mqtt_port != 1883 and not (Path(path).parent.parent / "secrets" / "mqtt_ca.pem").is_file():
    print("WARNING: MQTT TLS is selected but secrets/mqtt_ca.pem is absent; the built-in EMQX default CA will be used.")
PY

echo "PASS: local credentials, LoRa radio configuration, matching DER TLS certificate/key, and optional EST CA are provisioned."
