#!/usr/bin/env python3
"""Operator-assisted HIL fixture using the existing USB serial and Wi-Fi links.

No fixture hardware is controlled by this script. RF attenuation, power removal,
SD recovery and BLE peer actions are performed by the operator on the planned
fixture; this runner records prompts, serial output and optional Wi-Fi checks.
"""
import argparse
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

CASES = [
    ("rf_loss", "Apply the planned RF attenuator / simulate RF loss, then restore RF.", r"(radio|lora|recover|retry)"),
    ("reordering", "Use the existing test peer to deliver telemetry out of order.", r"(sequence|reorder|telemetry|duplicate)"),
    ("power_loss", "Remove and restore device power; confirm durable records recover.", r"(boot|recover|spool|journal)"),
    ("sd_recovery", "Make SD unavailable, restore it, and verify spool recovery.", r"(sd|storage|spool|degraded|recover)"),
    ("ble_interop", "Pair/reconnect using the approved BLE peer and verify encryption.", r"(ble|bond|pair|encrypt|sensor)"),
    ("codec", "Exercise audio capture/playback and inspect codec output.", r"(codec|audio|wm8962|sample)"),
    ("pps", "Feed/observe the existing GNSS PPS signal and verify timestamp handling.", r"(pps|gnss|gps|time)"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB serial port, e.g. /dev/ttyACM0 or COM5")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default="hil-results.json")
    parser.add_argument("--wifi-url", help="Optional existing device health/status URL")
    parser.add_argument("--timeout", type=float, default=3.0, help="Serial capture seconds per case")
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("Missing dependency: pip install pyserial", file=sys.stderr)
        return 2

    wifi_result = {"url": args.wifi_url, "ok": None, "status": None, "error": None}
    if args.wifi_url:
        try:
            from urllib.request import urlopen
            with urlopen(args.wifi_url, timeout=5) as response:
                wifi_result["status"] = response.status
                wifi_result["ok"] = 200 <= response.status < 400
        except Exception as exc:  # preserve error in report for operator review
            wifi_result["ok"] = False
            wifi_result["error"] = str(exc)

    results = {"started_utc": datetime.now(timezone.utc).isoformat(),
               "port": args.port, "baud": args.baud, "wifi_check": wifi_result,
               "cases": []}
    with serial.Serial(args.port, args.baud, timeout=0.2) as device:
        time.sleep(1.0)
        device.reset_input_buffer()
        for case_id, instruction, marker in CASES:
            print(f"\n[{case_id}] {instruction}")
            verdict = input("Perform the stimulus, then enter PASS, FAIL, or SKIP: ").strip().upper()
            start = time.monotonic()
            captured = bytearray()
            while time.monotonic() - start < args.timeout:
                chunk = device.read(4096)
                if chunk:
                    captured.extend(chunk)
            text = captured.decode("utf-8", errors="replace")
            matched = bool(re.search(marker, text, flags=re.IGNORECASE))
            results["cases"].append({"id": case_id, "operator_verdict": verdict,
                                     "expected_log_regex": marker, "log_marker_found": matched,
                                     "serial_capture": text})
            print(f"  captured={len(captured)} bytes; expected marker found={matched}")
            if verdict not in {"PASS", "FAIL", "SKIP"}:
                print("  Invalid verdict; recorded as FAIL")
                results["cases"][-1]["operator_verdict"] = "FAIL"
    results["finished_utc"] = datetime.now(timezone.utc).isoformat()
    output = Path(args.out)
    output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {output.resolve()}")
    return 1 if any(c["operator_verdict"] == "FAIL" for c in results["cases"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
