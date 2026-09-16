#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SECRETS="$ROOT/secrets"
LOCAL_CONFIG="$ROOT/include/LocalConfig.h"
CERT="$SECRETS/web_tls_cert.der"
KEY="$SECRETS/web_tls_key.der"

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

# Validate the local credential contract without printing any secret values.
python3 - "$LOCAL_CONFIG" <<'PY'
import re
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()

def macro(name):
    m = re.search(r'^\s*#define\s+' + re.escape(name) + r'\s+"([^"]*)"\s*$', text, re.M)
    return m.group(1) if m else None

ap_ssid = macro("FIELDRADIO_AP_SSID")
ap_password = macro("FIELDRADIO_AP_PASSWORD")
web_user = macro("FIELDRADIO_WEB_USER")
web_password = macro("FIELDRADIO_WEB_PASSWORD")
lora_key = macro("FIELDRADIO_LORA_KEY_HEX")

if not ap_ssid or len(ap_ssid) > 32:
    raise SystemExit("ERROR: FIELDRADIO_AP_SSID is missing or longer than 32 bytes.")
if not ap_password or not 8 <= len(ap_password) <= 63:
    raise SystemExit("ERROR: FIELDRADIO_AP_PASSWORD must be 8..63 characters.")
if not web_user or len(web_user) > 32:
    raise SystemExit("ERROR: FIELDRADIO_WEB_USER is missing or longer than 32 characters.")
if not web_password or not 8 <= len(web_password) <= 63:
    raise SystemExit("ERROR: FIELDRADIO_WEB_PASSWORD must be 8..63 characters.")
if ap_password == web_password:
    raise SystemExit("ERROR: AP and WebUI passwords must be different.")
if not lora_key or not re.fullmatch(r"[0-9A-Fa-f]{32}", lora_key):
    raise SystemExit("ERROR: FIELDRADIO_LORA_KEY_HEX must contain exactly 32 hexadecimal characters.")
PY

echo "PASS: local credentials and matching DER TLS certificate/key are provisioned."
