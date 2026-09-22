# Project status — pre-fabrication / pre-deployment

**Status baseline: 2026-09-17**

- **Belum ada node yang beroperasi.**
- **PCB belum diproduksi.**
- Mapping GPIO Rev-C masih berstatus **candidate / pre-fabrication routing contract**.
- **Rolling/perubahan pin masih diterima selama belum ada keterangan eksplisit bahwa PCB sudah diproduksi.** Setelah PCB dinyatakan diproduksi, perubahan pin harus diperlakukan sebagai hardware change dan tidak boleh dinormalisasi hanya dari firmware.
- Tidak ada klaim pada repository ini bahwa sebuah node lapangan sudah aktif atau bahwa PCB Rev-C sudah menjadi hardware produksi.
- Dokumen test matrix dan bring-up adalah **rencana/validasi tertunda**, bukan bukti hardware telah diuji.
- Setiap teks lama yang menyebut `supplied PCB`, `field deployment`, `production-final`, atau `Rev-C` harus dibaca dalam konteks ini: Rev-C adalah **target routing/design revision**, bukan bukti bahwa PCB fisik telah diproduksi.

## Status fitur

| Area | Status repository | Catatan |
|---|---|---|
| Core P2P LoRa, GNSS, SD, audio, WebUI HTTPS | Implemented in firmware | Hardware bring-up tetap tertunda |
| LoRaWAN Class A | Implemented/provisionable | RF/regulatory and network validation remain pending |
| GNSS PPS GPIO9 + 12-hour sync | Implemented in firmware | Hardware PPS validation pending |
| MAX17048 polling + ADC fallback | Implemented in firmware | Hardware/I2C validation pending |
| BLE Sensor Reader | **Implemented / hardware-validation pending** | NimBLE central membaca descriptor + notification sensor dinamis; perlu node BLE GATT nyata untuk validasi lapangan |
| Wi-Fi STA | **Partial / scaffold** | Manager/reconnect path ada, tetapi provisioning/configuration workflow end-to-end belum tersedia |
| MQTT | **Production security workflow implemented (FASE 3)** | Credential provisioning, production-build separation, TLS handshake timeout, CA verification, audit logging, rotation/warning controls are present; deployment still requires the documented production provisioning workflow |
| X25519/ECDH key rotation | **Implemented behind runtime policy; HIL pending** | V5 framing, authenticated beacon/key derivation, epoch retention, and runtime policy are implemented; interoperability and multi-node hardware validation remain pending |
| Fragment selective-repeat / full SACK | **Implemented in firmware; HIL pending** | Window 8, bounded 8-bit SACK, per-fragment retry and hard 2048-byte/16-fragment limits; real RF loss testing remains pending |
| Secure Boot + flash encryption production provisioning | **Procedure only** | Sengaja tidak diaktifkan otomatis |
| HIL/factory test/fuzz/native unit test | **Not complete** | Repository belum memiliki hardware fixture/test harness lengkap |

Status ini adalah baseline dokumentasi; jangan mengubahnya menjadi “production”, “deployed”, atau “hardware validated” tanpa bukti baru yang eksplisit.

## D-06 PKI lifecycle status (2026-09-22)

Decision is locked to automated RFC 7030 EST with a factory bootstrap
certificate. Firmware support is implemented in `EstClient` and
`CertLifecycleManager`; automatic lifecycle remains disabled by default.

The repository includes native certificate/CSR/expiry tests and an HIL procedure
for certificate NVS A/B power-loss validation. Target ESP32-S3 build validation
must still be executed in the PlatformIO/CI environment because PlatformIO is
not installed in this audit runtime.
