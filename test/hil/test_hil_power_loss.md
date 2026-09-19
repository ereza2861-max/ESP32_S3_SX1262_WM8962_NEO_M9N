# HIL: power loss

## Procedure
1. Generate continuous BLE sensor traffic.
2. Cut power at random points while the gateway is appending to the SD spool.
3. Reboot and inspect `/SENSOR/SPOOL.Q` through the WebUI status endpoint.

## Acceptance criteria
- A torn final record is discarded.
- All preceding CRC32-valid records remain recoverable.
- No malformed record causes a crash or out-of-bounds read.
- Planned `reboot` flushes/validates the spool before reset.

TODO(hw): brownout test with the production power rail and SD card used in the enclosure.
