# QnA decision patch notes

Source: `QnA.txt`, decisions D-01 through D-03 and the SAFE FIXES section.

Applied:
- D-01 = C: authenticated production serial console.
- GAP-001: health monitor state array covers all seven heartbeat sources.
- GAP-005: active version documentation is normalized to pioarduino 55.03.39,
  Arduino-ESP32 3.3.9, and ESP-IDF 5.5.4.
- GAP-006: stale active status wording is corrected in the active decision/readme
  material.

Selected but not falsely marked complete:
- D-02 = A requires converting all remaining direct `gConfig` readers to
  mutex-protected snapshots. Existing `configSnapshot()` infrastructure is
  already present, but this repository still contains direct readers.
- D-03 = A is implemented for sensor-spool delivery: the firmware emits MQTT
  QoS 1 directly on the transport used by PubSubClient and requires the matching
  PUBACK before clearing the MQTT delivery obligation. PubSubClient 2.8 remains
  responsible for the connection lifecycle because it does not expose a QoS-1
  publish API.

Not included as speculative changes:
- HIL acceptance, target builds, ECDH interoperability, and other validation
  groups explicitly marked NOT VERIFIED in the QnA source.


## D-04 through D-06

Applied from `QnA.txt`:
- D-04 = B: dedicated 16 kHz AEC working path with synchronized playback-reference resampling.
- D-05 = C: Codec2 1600 bit/s, 8 kHz, 40 ms voice frames.
- D-06 = A: full automated PKI lifecycle using RFC 7030 EST.
- EST architecture = D: firmware talks to a vendor-neutral EST endpoint or backend adapter.
- Initial enrollment authentication = 3: factory bootstrap certificate.

D-05 changes the voice application payload format and therefore requires all voice
nodes to use the same locked Codec2 mode/version. It does not change the outer
authenticated LoRa packet envelope.

D-06 removes password rotation as the primary MQTT credential mechanism. The
factory certificate bootstraps EST mTLS; renewal uses a newly generated P-256 key
and CSR, and the issued certificate/key are committed atomically.

## CONFIG_VERSION / ATOMIC_CONFIG_SCHEMA migration matrix

| CONFIG_VERSION | ATOMIC_CONFIG_SCHEMA | Meaning |
|---:|---:|---|
| 9 | 1 | Existing runtime configuration; no EST lifecycle fields |
| 10 | 2 | Adds EST endpoint/auth/lifecycle fields and extends the atomic payload |
| future | 2+ | Must introduce a new schema when the atomic payload layout changes |

Legacy per-key NVS fields are still read on boot. A configuration loaded from
the legacy representation is normalized and written into the current atomic
schema. A schema mismatch never gets interpreted as the current payload layout.
