# Changelog

## 2026-09-30 — FRAM → MRAM storage reconstruction
- Migrated ReplayStore from the obsolete I2C FRAM backend to native Everspin MR25H256 SPI MRAM.
- Migrated PersistentConfig runtime storage to MRAM A/B slots with commit-last atomicity and an `MRM1` migration marker.
- Added native `MramStorage` driver at 40 MHz SPI Mode 0.
- Reassigned GPIO42 exclusively to MRAM CS and removed the obsolete GPIO42 TX indicator.
- Retained GPIO39 addressable RGB TX/RX indication.
- Retained NVS as migration source/pre-authority fallback and did not delete NVS data.
- Kept credential-bearing state in encrypted NVS rather than plaintext MRAM.
- Preserved Secure Boot, Flash Encryption, and NVS Encryption.
- Added MRAM migration documentation and storage invariants.
- LoRa V2/V3/V4/V5 framing, fragment/ACK behavior, and ECDH beacon protocol are unchanged.



## 2026-09-19 — G13–G20
- Added dependency/version audit and explicit Espressif32 6.13.0 platform pinning.
- Added per-node BLE passkey provisioning, encrypted peer storage, identity/RPA handling, and sensor queue saturation policy.
- Added ESP32-C3 sensor-node environment, tests, and documentation for the BLE sensor architecture.
