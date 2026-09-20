#!/usr/bin/env python3
"""Audit dependency pinning without inventing versions."""
from pathlib import Path
import re, sys

ROOT = Path(__file__).resolve().parents[1]
files = [
    ROOT / "platformio.ini",
    ROOT / "sensor_node_esp32c3" / "platformio.ini",
    ROOT / "sdkconfig.defaults",
    ROOT / "src" / "idf_component.yml",
]
todos = []
for path in files:
    if not path.exists():
        continue
    in_lib_deps = False
    for no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if "TODO(pin)" in line:
            todos.append(f"{path.relative_to(ROOT)}:{no}: {line.strip()}")
        if path.name == "platformio.ini":
            stripped = line.strip()
            if stripped.startswith("[") and stripped.endswith("]"):
                in_lib_deps = False
            elif stripped == "lib_deps =":
                in_lib_deps = True
            elif in_lib_deps and stripped and not stripped.startswith(";") and not stripped.startswith("#"):
                if "<COMMIT_SHA>" in stripped:
                    todos.append(f"{path.relative_to(ROOT)}:{no}: unresolved commit placeholder: {stripped}")
                elif "/" in stripped and "@" not in stripped and not stripped.startswith("http"):
                    todos.append(f"{path.relative_to(ROOT)}:{no}: unpinned dependency: {stripped}")
                elif "@^" in stripped or "@~" in stripped:
                    todos.append(f"{path.relative_to(ROOT)}:{no}: non-immutable dependency range: {stripped}")

for item in todos:
    print(item)
if todos:
    print(f"audit_versions: {len(todos)} unresolved pin(s)")
    raise SystemExit(1)
print("audit_versions: all inspected entries are pinned")
