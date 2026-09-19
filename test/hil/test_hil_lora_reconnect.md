# HIL: LoRa reconnect

## Procedure
1. Generate sensor traffic while the LoRa downstream is unavailable.
2. Restore the radio path and verify pending samples drain from `/SENSOR/SPOOL.Q`.
3. Interrupt the link after a transmit but before its acknowledgement and restore it.

## Acceptance criteria
- Samples accepted by the BLE queue are persisted before downstream I/O.
- A successful LoRa delivery clears only the LoRa delivery bit.
- Retried samples do not corrupt the spool and the existing LoRa deduplication path remains active.
- `GET /api/sensors/spool` reports depth and eviction/drop counters.

TODO(hw): validate the exact radio failure/ACK-loss behavior on the target modem.
