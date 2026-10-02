Berikut keputusan final (cannonical decisions) yang saya susun sebagai referensi:

· Tidak menghapus/membongkar ulang implementasi firmware yang sudah ada.
· Hanya melengkapi atau meng-upgrade ke arah yang lebih baik.
· Tidak menambah hardware baru.
· Tidak menambah penggunaan pin baru (pin sudah sesak).

---

KEPUTUSAN FINAL

Prinsip Umum

1. Semua keputusan harus incremental terhadap firmware yang ada.
2. Tidak ada perubahan yang membatalkan kompatibilitas wire protocol yang sudah ada, kecuali ada versi baru yang additive dan backward-compatible.
3. Tidak boleh menambah pin, tidak boleh menambah komponen hardware, tidak boleh mengubah BoardConfig.h untuk pin baru.
4. Semua perubahan harus dapat diuji tanpa hardware baru.
5. Semua perubahan harus menjaga reproducibility build (PlatformIO) dan tetap lulus cppcheck/native test yang ada.

---

GROUP A — LoRa Node

Q01 — MQTT Command/Control

Keputusan: A — Tidak ada MQTT command channel.

Alasan:

· Menambah MQTT command channel akan memperluas attack surface, menambah state machine baru, dan membutuhkan backend contract baru.
· Audit sendiri menandai ini sebagai design-dependent.
· Firmware saat ini konsisten sebagai telemetry/health/availability publisher.

Konsekuensi:

· Tidak ada perubahan pada MqttClientManager untuk subscribe command.
· Dokumentasi harus menyatakan secara eksplisit: MQTT adalah satu arah (uplink) untuk telemetry, health, availability.

---

Q02 — Remote LoRa Sensor Telemetry saat MQTT Offline

Keputusan: B — Gunakan SensorSpool yang sudah ada untuk remote telemetry, dengan diskriminator sumber.

Alasan:

· Tidak menambah hardware, tidak menambah storage baru.
· SensorSpool sudah ada dan durable.
· Perubahan hanya menambah metadata sumber (local BLE vs remote LoRa) pada record spool.

Konsekuensi:

· SensorSpool::DiskRecord perlu field source (enum: LOCAL_BLE=0, REMOTE_LORA=1).
· SensorSpool::append menerima parameter sumber.
· taskSensorForward mengarahkan remote LoRa telemetry ke spool juga.
· Tidak mengubah format BLE/LoRa wire protocol.

---

Q03 — MQTT Delivery Contract

Keputusan: A — QoS-1 hanya untuk sensor samples; telemetry/health/availability best-effort.

Alasan:

· Sudah menjadi perilaku saat ini.
· Menyeragamkan QoS-1 untuk semua telemetry membutuhkan perubahan besar pada PubSubClient wrapper.
· Tidak ada kebutuhan yang terbukti untuk QoS-1 pada health/availability.

Konsekuensi:

· Dokumentasikan matriks QoS secara eksplisit di docs/MQTT_ROTATION_CONTRACT.md atau dokumen baru docs/MQTT_QOS_MATRIX.md.
· Tidak ada perubahan kode wajib, hanya dokumentasi dan assertion.

---

GROUP B — Sensor Node

Q04 — Timestamp Authority

Keputusan: A — Pertahankan gateway receipt timestamp.

Alasan:

· Menambah RTC/time-sync ke sensor node = kompleksitas besar, tidak ada hardware RTC, tidak ada pin untuk PPS.
· Gateway timestamp sudah didokumentasikan sebagai kontrak (Q-B03(A)).
· Tidak ada kebutuhan fisik yang terbukti untuk measurement-time timestamp.

Konsekuensi:

· Tidak ada perubahan BLE wire.
· Dokumentasi harus tegas: timestamp adalah gateway receipt time, bukan measurement time.
· Tambahkan metadata QUALITY_TIMESTAMP_GATEWAY yang sudah ada, dan pastikan selalu diset saat sensor mengirim timestamp = 0.

---

Q05 — Sensor Node Command Protocol

Keputusan: B — BLE command channel, minimal dan backward-compatible.

Alasan:

· BLE sudah ada dan sudah punya GATT service.
· Menambah command characteristic bersifat additive.
· Tidak menambah pin, tidak menambah hardware.
· Tidak memaksa LoRa command channel.

Konsekuensi:

· Tambahkan karakteristik BLE baru: COMMAND_UUID dan COMMAND_RESPONSE_UUID.
· Command minimal: SET_SAMPLING_PERIOD, REQUEST_DESCRIPTOR_REFRESH, REQUEST_SENSOR_RESET.
· Semua command harus melalui BLE authentication yang sudah ada.
· Tidak mengubah descriptor request yang sudah ada.
· Sensor node tetap berfungsi jika gateway tidak menggunakan command channel.

---

Q06 — Sensor Failure Model

Keputusan: B — Explicit DEGRADED state, tanpa state machine penuh.

Alasan:

· State machine penuh (ERROR → RECOVERY → READY) menambah kompleksitas besar.
· DEGRADED cukup untuk merepresentasikan kondisi roster tidak lengkap atau driver gagal.
· Bisa diimplementasikan sebagai flag di SensorRegistry dan diekspos lewat BLE descriptor flags atau status characteristic.

Konsekuensi:

· Tambahkan SensorProtocol::FLAG_DEGRADED pada descriptor flags.
· Sensor node menandai descriptor sebagai degraded jika driver read() gagal berulang.
· Tidak ada perubahan wire format struct, hanya flag bit baru.
· Dokumentasi harus menyatakan DEGRADED sebagai kondisi per-sensor, bukan per-node.

---

Q07 — Sensor Calibration

Keputusan: B — Factory calibration only, disimpan di NVS sensor node, tanpa runtime calibration command.

Alasan:

· Factory calibration saja sudah cukup untuk deployment awal.
· Runtime calibration command menambah state machine dan command channel.
· NVS sudah ada di sensor node.

Konsekuensi:

· Tambahkan namespace NVS calib di sensor node.
· SensorDriverRegistry membaca scale/offset dari NVS saat begin().
· Nilai default tetap dari descriptor.
· Tidak ada UI kalibrasi runtime.
· Dokumentasi harus menyatakan calibration adalah factory-only.

---

Q08 — Sensor Sampling Scheduler

Keputusan: B — Scheduler tick diturunkan dari minimum periodMs, tanpa task per sensor.

Alasan:

· Tick per-sensor terlalu besar overhead di ESP32-C3.
· Menurunkan tick dari minimum periodMs adalah perubahan kecil dan aman.
· Tetap satu loop, satu registry.

Konsekuensi:

· main.cpp menghitung minPeriodMs dari descriptor setelah roster dibangun.
· Loop menggunakan minPeriodMs sebagai tick, dengan batas bawah SENSOR_SAMPLE_PERIOD_MS.
· Tidak ada perubahan API driver.
· Dokumentasi harus menyatakan jaminan: periodMs >= tick.

---

GROUP C — LoRa ↔ Sensor

Q09 — Sensor Identity

Keputusan: A — Pertahankan BLE-address-derived node ID.

Alasan:

· Sudah ada, deterministik, tidak menambah field baru di wire.
· UUID provisioning akan menambah state dan storage.

Konsekuensi:

· Tidak ada perubahan pada nodeIdFromAddress.
· Dokumentasi harus menyatakan node ID adalah turunan alamat BLE, dan replacement sensor node akan menghasilkan node ID baru.

---

Q10 — Telemetry Versioning

Keputusan: C — Tambahkan schema version + firmware version, secara additive pada BLE descriptor dan pada record LoRa sensor telemetry.

Alasan:

· Tidak mengubah struct SensorValue yang sudah ada.
· Bisa ditambahkan sebagai field baru di SensorDescriptor dan di payload LoRa sensor telemetry dengan cara yang backward-compatible.

Konsekuensi:

· Tambahkan SensorProtocol::FLAG_HAS_SCHEMA_VERSION dan field schemaVersion di SensorDescriptor.
· SensorTelemetry::serializeSensorTelemetry diperluas dengan field schema + firmware version di area padding yang sudah ada (18 byte padding), sehingga tidak mengubah PAYLOAD_BYTES.
· Firmware version diambil dari konstanta build.
· Backward-compatible: penerima lama mengabaikan padding.

---

Q11 — Ordering

Keputusan: C — Tambahkan monotonically increasing source sequence + timestamp.

Alasan:

· sampleId bukan sequence.
· Sequence dibutuhkan untuk deteksi gap dan deduplikasi kuat.
· Bisa ditambahkan secara additive.

Konsekuensi:

· Tambahkan uint32_t sourceSequence di SensorValue dengan cara mengganti padding atau menambah field baru dengan flag.
· Karena SensorValue sudah 15 byte, tambahkan field baru di struct baru SensorValueV2 atau gunakan flag untuk mengaktifkan interpretasi baru.
· Untuk LoRa sensor telemetry, gunakan area padding 18 byte untuk menyimpan sourceSequence.
· Sequence dinaikkan di sensor node per sampel.
· Tidak ada perubahan pada SensorValue V1; V2 ditandai dengan flag.

---

Q12 — Sensor Command Acknowledgement

Keputusan: C — ACK + result + error code + sequence, hanya untuk BLE command channel.

Alasan:

· Karena Q05 memilih BLE command channel, ACK harus lengkap agar idempoten.
· Tidak menambah hardware.
· Tidak memaksa LoRa command channel.

Konsekuensi:

· CommandResponse struct baru: commandId, sequence, result, errorCode.
· Dikirim via COMMAND_RESPONSE_UUID.
· Timeout dan duplicate handling di gateway.
· Sensor node menyimpan lastCommandSequence per peer.

---

Q13 — Duplicate Handling

Keputusan: C — Sequence + persistent receiver deduplication, hanya di gateway untuk jalur LoRa sensor telemetry.

Alasan:

· Gateway sudah punya dedupCache_ untuk LoRa.
· Bisa diperluas untuk sensor telemetry dengan key (sourceId, sourceSequence).
· Tidak menambah hardware.

Konsekuensi:

· Tambahkan dedup khusus sensor telemetry di LoRaManager menggunakan sourceSequence jika tersedia.
· Jika tidak tersedia (backward-compatible), fallback ke sampleId.
· Tidak mengubah dedup LoRa umum.

---

GROUP D — Security

Q14 — ECDH Key-at-Rest

Keputusan: C — Pertahankan arsitektur saat ini, tetapi blokir enablement produksi ECDH sampai ada bukti provisioning (Secure Boot + Flash Encryption + eFuse).

Alasan:

· Tidak menambah hardware secure element.
· Tidak mengubah arsitektur.
· Hanya menambahkan gate runtime.

Konsekuensi:

· RuntimeConfig::ecdhRekeyPolicy tetap ada.
· Tambahkan pemeriksaan: jika ecdhRekeyPolicy == 1 dan Secure Boot/Flash Encryption tidak aktif, tolak aktivasi dan log error.
· Dokumentasi harus menyatakan ECDH produksi memerlukan provisioning security yang terbukti.

---

Q15 — Sensor OTA Authorization Policy

Keputusan: B — Samakan autentikasi OTA dan profile: password + session token; pertahankan konfirmasi kabel sebagai syarat terpisah.

Alasan:

· Firmware upload dan profile change sama-sama operasi administratif.
· Session token sudah ada.
· Konfirmasi kabel fisik tetap wajib untuk profile karena melibatkan perubahan hardware mapping.

Konsekuensi:

· handleUpload dan handleUploadDone juga memerlukan X-OTA-Session.
· handleProfile tetap memerlukan X-Profile-Cables-Changed.
· Tidak ada perubahan wire BLE.
· Dokumentasi harus menyatakan policy secara eksplisit.

---

GROUP E — Hardware

Q16 — Charging State

Keputusan: A — Pertahankan heuristik "charge probable".

Alasan:

· Tidak menambah pin.
· Tidak menambah IC charger telemetry.
· Hardware saat ini tidak punya STAT input.

Konsekuensi:

· Tidak ada perubahan kode.
· Dokumentasi harus tegas: charging adalah heuristik, bukan pengukuran.

---

Q17 — GPIO3 Sensor Multiplexing

Keputusan: A — Pertahankan arsitektur mux saat ini.

Alasan:

· Pin sudah sesak.
· Mengganti ke dedicated GPIO tidak mungkin tanpa hardware baru.
· Firmware sudah punya serialization.

Konsekuensi:

· Tidak ada perubahan pin.
· Dokumentasi harus menyatakan mux adalah kontrak hardware final.

---

GROUP F — Reliability

Q18 — Sensor Watchdog

Keputusan: C — Watchdog + persistent boot-loop recovery state, tanpa hardware tambahan.

Alasan:

· ESP32-C3 sudah punya Task WDT bawaan IDF.
· Tidak perlu hardware eksternal.
· Boot-loop recovery state mencegah loop tak terbatas.

Konsekuensi:

· Aktifkan Task WDT di sensor node.
· Tambahkan NVS key boot_loop_count.
· Jika boot loop > threshold, matikan BLE dan hanya nyalakan OTA AP terbatas, atau tunggu intervensi serial.
· Fatal init loop diganti dengan recovery cycle.

---

Q19 — Remote Telemetry Durability

Keputusan: C — Durable spool dengan bounded priority/eviction policy, memakai SensorSpool yang ada.

Alasan:

· Sama dengan Q02.
· Prioritas dapat ditentukan dari jenis telemetry.

Konsekuensi:

· SensorSpool mendapat field priority.
· Eviction memilih record prioritas terendah lebih dulu.
· Tidak ada storage tambahan.

---

GROUP G — Testing

Q20 — Production Acceptance

Keputusan: C — Build + HIL + manufacturing security evidence.

Alasan:

· Audit menandai production acceptance sebagai OPEN.
· Tidak menambah hardware, hanya menambah prosedur dan dokumentasi.
· HIL sudah ada prosedurnya.

Konsekuensi:

· Tambahkan docs/PRODUCTION_ACCEPTANCE_MATRIX.md.
· Tambahkan tools/check-production-acceptance.sh yang memverifikasi:
  · build artifact hash,
  · eFuse state,
  · Secure Boot enabled,
  · Flash Encryption enabled,
  · device certificate terpasang.
· Tidak mengubah firmware runtime.

---

Ringkasan Keputusan

ID Keputusan
Q01 A — MQTT telemetry-only
Q02 B — Pakai SensorSpool untuk remote telemetry
Q03 A — QoS-1 hanya sensor samples
Q04 A — Gateway timestamp
Q05 B — BLE command channel minimal
Q06 B — Flag DEGRADED per sensor
Q07 B — Factory calibration di NVS
Q08 B — Tick dari minimum periodMs
Q09 A — BLE-address-derived node ID
Q10 C — Schema + firmware version, additive
Q11 C — Sequence + timestamp, additive
Q12 C — ACK + result + error + sequence
Q13 C — Sequence + persistent dedup di gateway
Q14 C — Blokir ECDH produksi sampai provisioning evidence
Q15 B — Autentikasi uniform + konfirmasi kabel
Q16 A — Heuristik charging
Q17 A — Pertahankan mux GPIO3
Q18 C — Watchdog + boot-loop recovery
Q19 C — Durable spool + priority
Q20 C — Build + HIL + manufacturing evidence

---