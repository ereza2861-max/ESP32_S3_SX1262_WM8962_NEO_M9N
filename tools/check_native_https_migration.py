#!/usr/bin/env python3
"""Static sanity checks for the native ESP-IDF HTTPS migration."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
required = [
    "HttpdServer.h", "HttpdServer.cpp",
    "HttpdRequest.h", "HttpdRequest.cpp",
    "HttpdResponse.h", "HttpdResponse.cpp",
    "HttpdMultipart.h", "HttpdMultipart.cpp",
    "WebUi.h", "WebUi.cpp",
]
missing = [name for name in required if not (SRC / name).is_file()]
if missing:
    print("missing required files:", ", ".join(missing), file=sys.stderr)
    sys.exit(1)

all_src = "\n".join(p.read_text(errors="replace") for p in SRC.rglob("*") if p.is_file())
if "ESPWebServerSecure" in all_src:
    print("legacy ESPWebServerSecure reference remains under src/", file=sys.stderr)
    sys.exit(1)

webui = (SRC / "WebUi.cpp").read_text()
pairs = set(re.findall(r'\breg\(\s*"([^"]+)"\s*,\s*HTTP_(GET|POST|DELETE|PUT)', webui))
if len(pairs) != 121:
    print(f"expected 121 distinct URI/method pairs, found {len(pairs)}", file=sys.stderr)
    sys.exit(1)

for required_pair in [
    ("/api/message", "POST"),
    ("/api/ecdh/status", "GET"),
    ("/api/upload", "POST"),
]:
    if required_pair not in pairs:
        print("missing endpoint:", required_pair, file=sys.stderr)
        sys.exit(1)

ini = (ROOT / "platformio.ini").read_text()
if "esp32_idf5_https_server_compat" in ini:
    print("legacy HTTPS compatibility dependency remains in platformio.ini", file=sys.stderr)
    sys.exit(1)

print(f"native HTTPS static checks passed: {len(required)} files, {len(pairs)} URI/method pairs")
