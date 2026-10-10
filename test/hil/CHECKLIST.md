# HIL acceptance checklist (greenfield / prospective)

Run `python3 test/hil/run_hil.py --port <USB_SERIAL_PORT> [--wifi-url <existing-status-url>]`.
Install `pyserial` in the test environment first. This runner uses only the board's
existing USB Serial and Wi-Fi interfaces. It does not add or require hardware.

- [ ] RF loss simulation using the planned PCB attenuator; verify bounded retry and radio reinitialization without reboot.
- [ ] Telemetry reordering and duplicate replay; verify ordering/replay policy and durable ACK boundary.
- [ ] Power loss during queued telemetry; verify durable spool/journal recovery after restart.
- [ ] SD removal/unavailability and restoration; verify read-only degraded behavior and recovery.
- [ ] BLE interoperability with approved peer; verify encrypted bonding and manual re-pair/re-provision recovery.
- [ ] Codec capture/playback; verify expected sample format and no regression in existing audio features.
- [ ] GNSS PPS validation; verify PPS edge/timestamp correlation against the test fixture's reference.
- [ ] Archive `hil-results.json`, serial logs, firmware hashes, board revision and operator identity with the acceptance record.

The script records operator verdicts and serial output. Log marker matching is advisory,
not proof of electrical accuracy. Hardware validation remains a release gate until all
measurements and the manual checklist are reviewed and signed off.
