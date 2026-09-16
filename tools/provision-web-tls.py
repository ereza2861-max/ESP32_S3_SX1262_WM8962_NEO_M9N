from pathlib import Path

Import("env")

ROOT = Path(env.subst("$PROJECT_DIR"))
OUT = ROOT / "include" / "generated" / "WebTlsProvisioning.h"
CERT = ROOT / "secrets" / "web_tls_cert.der"
KEY = ROOT / "secrets" / "web_tls_key.der"

def emit_array(name, data):
    lines = []
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        lines.append("    " + chunk)
    return f"static const uint8_t {name}[] = {{\n" + ",\n".join(lines) + "\n};\n"

OUT.parent.mkdir(parents=True, exist_ok=True)

import os
import subprocess

def validate_der(cert_path, key_path):
    if not cert_path.is_file() or not key_path.is_file():
        return False
    cert = cert_path.read_bytes()
    key = key_path.read_bytes()
    if len(cert) < 128 or len(key) < 64:
        raise RuntimeError("WebUI TLS DER material is implausibly small")
    try:
        subprocess.run(
            ["openssl", "x509", "-inform", "DER", "-in", str(cert_path), "-noout"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(
            ["openssl", "pkey", "-inform", "DER", "-in", str(key_path), "-noout"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        cert_pub = subprocess.run(
            ["openssl", "x509", "-inform", "DER", "-in", str(cert_path), "-pubkey", "-noout"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        key_pub = subprocess.run(
            ["openssl", "pkey", "-inform", "DER", "-in", str(key_path), "-pubout"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        if cert_pub != key_pub:
            raise RuntimeError("WebUI TLS certificate and private key do not match")
    except FileNotFoundError as exc:
        raise RuntimeError("openssl is required to validate WebUI TLS material") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError("invalid WebUI TLS DER material" + (f": {detail}" if detail else "")) from exc
    return True

if not validate_der(CERT, KEY):
    if CERT.exists() or KEY.exists():
        raise RuntimeError("WebUI TLS provisioning requires both secrets/web_tls_cert.der and secrets/web_tls_key.der")
    if os.environ.get("FIELDRADIO_REQUIRE_TLS") == "1":
        raise RuntimeError("WebUI TLS provisioning is required; run 'make provision' first")
    OUT.write_text("""#pragma once\n#include <stdint.h>\n#define WEB_TLS_CERT_CONFIGURED 0\nstatic const uint8_t* const WEB_TLS_CERT_DER = nullptr;\nstatic const uint8_t* const WEB_TLS_KEY_DER = nullptr;\nstatic constexpr unsigned WEB_TLS_CERT_DER_LEN = 0;\nstatic constexpr unsigned WEB_TLS_KEY_DER_LEN = 0;\n""")
    print("WebUI TLS provisioning: no local certificate material; HTTPS service will remain disabled (CI-safe build).")
else:
    cert = CERT.read_bytes()
    key = KEY.read_bytes()
    OUT.write_text("""#pragma once\n#include <stdint.h>\n#define WEB_TLS_CERT_CONFIGURED 1\n""" +
                   emit_array("WEB_TLS_CERT_DER", cert) +
                   emit_array("WEB_TLS_KEY_DER", key) +
                   f"static constexpr unsigned WEB_TLS_CERT_DER_LEN = {len(cert)};\n" +
                   f"static constexpr unsigned WEB_TLS_KEY_DER_LEN = {len(key)};\n")
    print(f"WebUI TLS provisioning: loaded {len(cert)}-byte certificate and {len(key)}-byte private key.")
