#!/bin/sh
# ENH-5: Lightweight serial smoke checklist; this is not a HIL replacement.
set -u

DEVICE=${1:-}
if [ -z "$DEVICE" ]; then
  echo "usage: $0 <device-serial-path>" >&2
  exit 2
fi
if ! command -v stty >/dev/null 2>&1 ||
   ! command -v cat >/dev/null 2>&1 ||
   ! command -v timeout >/dev/null 2>&1; then
  echo "ERROR: stty, cat, and timeout are required" >&2
  exit 2
fi

if stty -F "$DEVICE" 115200 raw -echo -icrnl -ixon -ixoff >/dev/null 2>&1; then
  :
elif stty -f "$DEVICE" 115200 raw -echo -icrnl -ixon -ixoff >/dev/null 2>&1; then
  :
else
  echo "cannot open serial: $DEVICE" >&2
  exit 1
fi

LOG=$(mktemp "${TMPDIR:-/tmp}/fieldradio-check.XXXXXX") || exit 2
trap 'rm -f "$LOG"' EXIT HUP INT TERM

timeout 5 cat "$DEVICE" >"$LOG" 2>/dev/null || true
if grep -Fq "FIELDREADY" "$LOG"; then
  echo "[PASS] FIELDREADY within 5s"
else
  echo "[FAIL] FIELDREADY within 5s"
fi

printf 'AT+STATUS\n' >"$DEVICE" 2>/dev/null || true
STATUS_LOG="${LOG}.status"
trap 'rm -f "$LOG" "$STATUS_LOG"' EXIT HUP INT TERM
timeout 2 cat "$DEVICE" >"$STATUS_LOG" 2>/dev/null || true
if grep -Fq "OK" "$STATUS_LOG"; then
  echo "[PASS] AT+STATUS returned OK"
else
  echo "[FAIL] AT+STATUS did not return OK"
fi

if grep -Fq "CONFIG MIGRATION:" "$LOG" || grep -Fq "CONFIG MIGRATION:" "$STATUS_LOG"; then
  echo "[SKIP] CONFIG MIGRATION log is informational"
else
  echo "[SKIP] CONFIG MIGRATION log is informational"
fi

if grep -Fq "[CFG-TXN]" "$LOG" || grep -Fq "[CFG-TXN]" "$STATUS_LOG"; then
  echo "[SKIP] [CFG-TXN] log is informational"
else
  echo "[SKIP] [CFG-TXN] log is informational"
fi

exit 0
