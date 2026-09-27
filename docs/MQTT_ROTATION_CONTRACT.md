# MQTT Credential Rotation Contract

STATUS: IMPLEMENTED — firmware-side PKI rotation uses the sequence below. The
EST server remains the credential authority; the repository does not embed a
broker-side administrative API or broker private key.

## Rotation sequence

```text
rotation deadline
        ↓
credential generation/renewal
        ↓
broker credential update
        ↓
secure persistence
        ↓
connection replacement
        ↓
old credential retirement
```

### 1. Rotation deadline

`certRenewalThresholdDays` is evaluated against the active certificate's
`notAfter`. An expired or threshold-reached certificate enters the renewal path.
The firmware records `ROTATION_DEADLINE_CHECK` in the certificate lifecycle log.

### 2. Credential generation/renewal

`EstClient::enroll()` generates a fresh private key and CSR and requests a new
certificate from the configured EST authority. The old key remains available
until the new credential has passed verification.

### 3. Broker credential update

The issued certificate/chain is accepted as the new broker credential only after
certificate parsing, subject/key-pair checks, and EST trust-chain verification.
The lifecycle records `ROTATION_BROKER_CREDENTIAL_UPDATE_ACCEPTED` only after
those checks succeed.

### 4. Secure persistence

The new certificate/key pair is written to the journaled A/B certificate slots
and read back before the slot is committed. The previous slot remains available
for rollback until the new connection succeeds.

### 5. Connection replacement

The old MQTT transport is explicitly disconnected and rebuilt with the newly
loaded certificate/key. The replacement connection is attempted by the normal
MQTT task. Retirement is not allowed merely because persistence succeeded.

### 6. Old credential retirement

Only after a successful MQTT connection using the new credential is the previous
certificate slot and legacy single-slot material removed. If replacement fails,
the previous committed slot remains available and the new connection can retry.

## Endpoint contract

A deployment may expose the following EST-side/broker-authority contract:

### POST `/v1/mqtt/rotate`

Request body:
- `device_id`
- `current_serial`
- `csr_pem`

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

The firmware's current production path uses EST enrollment rather than a
hard-coded `/v1/mqtt/rotate` HTTP client. If a deployment uses the endpoint above,
the server must preserve the same ordering guarantees: the new credential must be
accepted before the old credential is revoked.
