<!-- F1-TODO-1: Firmware-side MQTT certificate rotation preparation contract. -->
# MQTT Rotation Contract

STATUS: DRAFT — firmware-side preparation only. Backend endpoint belum ada. Firmware Fase 1 tidak memanggil ini.

## Endpoint minimal

### POST `/v1/mqtt/rotate`

Request body:
- `device_id`
- `current_serial`
- `csr_pem` (opsional)

Response:
- `200`: `cert_pem`, `ca_pem`, `serial`, `expires_at`, `grace_until`
- `403`: `device_not_authorized`
- `409`: `rotation_in_progress`
- `503`: `ca_unavailable`

### POST `/v1/mqtt/rotate/ack`

Request body:
- `device_id`
- `serial`

### GET `/v1/mqtt/status`

Response:
- `cert_serial`
- `expires_at`
- `rotation_pending`

## Non-goals Fase 1

Firmware TIDAK memanggil endpoint di atas. Fase 1 hanya menyiapkan state dan verifikasi material sertifikat. Tidak ada request HTTPS ke backend, tidak ada endpoint WebUI baru, dan tidak ada perubahan wire protocol.
