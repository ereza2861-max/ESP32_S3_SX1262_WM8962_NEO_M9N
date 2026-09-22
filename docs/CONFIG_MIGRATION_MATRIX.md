# Configuration migration matrix

The atomic configuration record uses `ATOMIC_CONFIG_SCHEMA_VERSION = 2`.
`CONFIG_VERSION` remains the human-visible legacy migration version.

| CONFIG_VERSION | Atomic schema | Meaning / field migration |
|---:|---:|---|
| 1 | 1 | Initial persisted runtime fields; no atomic A/B slot record. |
| 2 | 1 | Adds radio/audio and initial web credentials. |
| 3 | 1 | Adds MQTT runtime fields and retention settings. |
| 4 | 1 | Adds BLE pairing flag; initializes it from the compile-time default. |
| 5 | 1 | Adds deep-sleep and battery policy fields. |
| 6 | 1 | Adds LoRaWAN runtime fields. |
| 7 | 1 | Adds sensor-reader and BLE policy fields. |
| 8 | 1 | Adds web session/CSRF and replay-window policy. |
| 9 | 1 | Adds EST server/label and certificate lifecycle timing fields. |
| 10 | 2 | Uses atomic A/B configuration slots with generation + CRC and adds EST auth mode. |
| 11+ | 2 | Reserved for future fields; bump the schema if the atomic payload layout changes. |

## Current EST fields

`CONFIG_VERSION=10` / schema 2 stores `estAuthMode`, `estUsername`,
`estPassword`, and `estBootstrapToken` in the encrypted NVS configuration
boundary. Mode 2 bootstrap tokens are cleared after the first successful
enrollment.

## Migration procedure

1. Boot with the new firmware and allow `RuntimeConfig::load()` to read the
   existing configuration.
2. Validate the resulting candidate with `validRuntimeConfig()`.
3. Commit the complete candidate to the inactive atomic slot.
4. Verify the slot CRC and commit marker before selecting it.
5. After a successful commit, only the newest valid generation is selected.

There is no requirement for backward compatibility with an older atomic payload
layout; a schema change invalidates the old slots and requires the normal
provisioning/migration path.
