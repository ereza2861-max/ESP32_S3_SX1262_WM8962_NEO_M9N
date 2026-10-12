#!/usr/bin/env python3
"""Guardedly de-duplicate USB UAC descriptor registration in the local build tree."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD_LOG = ROOT / "build.log"
SIGNATURE = "Two environments with different actions were specified for the same target:"

if not BUILD_LOG.is_file() or SIGNATURE not in BUILD_LOG.read_text(
        encoding="utf-8", errors="replace"):
    print("USB UAC workaround refused: expected duplicate-action error not found.",
          file=sys.stderr)
    sys.exit(2)

managed = ROOT / ".pio" / "build" / "esp32s3" / "managed_components"
candidates = []
if managed.is_dir():
    for path in managed.rglob("CMakeLists.txt"):
        content = path.read_text(encoding="utf-8", errors="replace")
        if "usb_descriptors.c" in content and "target_sources" in content and "tusb_lib" in content:
            candidates.append(path)
if len(candidates) != 1:
    print(f"USB UAC workaround expected exactly one matching managed CMakeLists; found {len(candidates)}.",
          file=sys.stderr)
    sys.exit(3)

cmake_file = candidates[0]
text = cmake_file.read_text(encoding="utf-8")
# Remove only the target_sources registration that creates the duplicate
# SCons action. Preserve unrelated target_sources declarations.
source_pattern = re.compile(
    r"^[ \t]*target_sources\(\$\{tusb_lib\}[ \t]+PUBLIC[ \t]+"
    r"\"\$\{COMPONENT_DIR\}/tusb/usb_descriptors\.c\"[ \t]*\)[ \t]*\n?",
    re.MULTILINE,
)
updated, removed = source_pattern.subn("", text, count=1)
if removed != 1:
    print("USB UAC workaround refused: duplicate target_sources registration was not recognized.",
          file=sys.stderr)
    sys.exit(4)

# Ensure the descriptor is compiled by the component itself exactly once.
# Match the idf_component_register call across formatting differences.
match = re.search(r"idf_component_register\s*\((.*?)\)", updated, re.DOTALL)
if not match or not re.search(r"\bSRCS\b", match.group(1)):
    print("USB UAC workaround refused: idf_component_register(SRCS ...) not found.",
          file=sys.stderr)
    sys.exit(5)
block = match.group(1)
if "tusb/usb_descriptors.c" not in block:
    updated_block = re.sub(r"\bSRCS\b", 'SRCS "tusb/usb_descriptors.c"', block, count=1)
    updated = updated[:match.start(1)] + updated_block + updated[match.end(1):]

if updated == text:
    print("USB UAC workaround made no changes; refusing to claim success.", file=sys.stderr)
    sys.exit(6)
cmake_file.write_text(updated, encoding="utf-8")
print(f"Patched managed USB UAC component: {cmake_file}")
