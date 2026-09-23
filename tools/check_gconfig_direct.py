#!/usr/bin/env python3
"""Fail closed when subsystem code bypasses the RuntimeConfig snapshot contract."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCAN_ROOTS = (ROOT / "src", ROOT / "include")
ALLOWED = {
    ROOT / "src" / "PersistentConfig.cpp",
    ROOT / "include" / "PersistentConfig.h",
}
ROTATION_ALLOWED = ALLOWED | {ROOT / "src" / "WebUi.cpp"}
DIRECT = re.compile(r"\bgConfig\s*\.")
ROTATION = re.compile(r"\bmqttCredentialRotationDays\b", re.IGNORECASE)

errors: list[str] = []

for base in SCAN_ROOTS:
    for path in sorted(base.rglob("*")):
        if not path.is_file() or path in ALLOWED or path.suffix not in {".cpp", ".h", ".hpp", ".cc"}:
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError as exc:
            errors.append(f"{path.relative_to(ROOT)}: cannot decode as UTF-8: {exc}")
            continue
        for number, line in enumerate(lines, 1):
            if DIRECT.search(line):
                errors.append(
                    f"{path.relative_to(ROOT)}:{number}: direct gConfig. access; use configSnapshot(local)"
                )
            if ROTATION.search(line) and path not in ROTATION_ALLOWED:
                errors.append(
                    f"{path.relative_to(ROOT)}:{number}: mqttCredentialRotationDays is restricted to "
                    "PersistentConfig/WebUI compatibility handling; it must not drive credential rotation"
                )

if errors:
    print("ERROR: configuration ownership/static policy check failed:")
    for error in errors:
        print(f"  {error}")
    sys.exit(1)

print("OK: no direct subsystem gConfig. access and no non-WebUI rotation-policy use detected.")
