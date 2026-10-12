#!/usr/bin/env python3
"""Apply a narrow workaround for duplicate USB UAC descriptor compilation.

Run only after the known PlatformIO SCons error is detected. The managed
component is generated/downloaded by ESP-IDF, so this intentionally patches
the local .pio build copy rather than changing tracked source or credentials.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD_LOG = ROOT / "build.log"
SIGNATURE = "Two environments with different actions were specified for the same target:"

if not BUILD_LOG.is_file() or SIGNATURE not in BUILD_LOG.read_text(
        encoding="utf-8", errors="replace"):
    print("USB UAC workaround refused: expected duplicate-action error not found.",
          file=sys.stderr)
    sys.exit(2)

candidates = list((ROOT / ".pio" / "build").glob(
    "esp32s3/managed_components/**/usb_device_uac/CMakeLists.txt"))
if not candidates:
    # Component Manager naming may differ between PlatformIO/ESP-IDF releases.
    candidates = list((ROOT / ".pio" / "build").glob(
        "esp32s3/managed_components/**/CMakeLists.txt"))
    candidates = [p for p in candidates
                  if "target_sources(${tusb_lib} PUBLIC" in
                  p.read_text(encoding="utf-8", errors="replace")
                  and "usb_descriptors.c" in
                  p.read_text(encoding="utf-8", errors="replace")]

if len(candidates) != 1:
    print(f"USB UAC workaround expected exactly one component CMakeLists; "
          f"found {len(candidates)}.", file=sys.stderr)
    sys.exit(3)

cmake_file = candidates[0]
text = cmake_file.read_text(encoding="utf-8")
source_line = 'target_sources(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb/usb_descriptors.c")'
if source_line not in text:
    print("USB UAC workaround refused: upstream CMake layout is not recognized.",
          file=sys.stderr)
    sys.exit(4)

register_old = 'idf_component_register(SRCS usb_device_uac.c\n                    INCLUDE_DIRS "include"'
register_new = ('idf_component_register(SRCS usb_device_uac.c "tusb/usb_descriptors.c"\n'
                '                    INCLUDE_DIRS "include" "tusb" "tusb_uac"')
if register_old not in text:
    print("USB UAC workaround refused: component registration layout is not recognized.",
          file=sys.stderr)
    sys.exit(5)

# Compile the descriptor source in the UAC component exactly once. Keep the
# TinyUSB include directories, but do not register the same source a second
# time on the TinyUSB target (which collides in PlatformIO's SCons builder).
text = text.replace(register_old, register_new, 1)
text = text.replace(source_line, "", 1)
cmake_file.write_text(text, encoding="utf-8")
print(f"Patched managed USB UAC component: {cmake_file}")
