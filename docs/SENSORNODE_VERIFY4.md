# FASE 3 — GAP-3 Verification

## A. Patch integrity

```bash
git apply --check PATCH3.diff
git apply PATCH3.diff
git diff --check
```

Expected: no errors.

## B. Production compile-time behavior

Build with:

```text
-DFIELDRADIO_PRODUCTION_BUILD
```

Expected:
- firmware compiles;
- no compile-time MQTT host is required;
- `Config::MQTT_HOST` is an empty string in this build.

## C. Missing credentials must not connect

On a fresh/provision-reset device with no `mqtt_creds/blob` and no
`provisioned=true`:

```text
MqttClientManager::begin() -> false
```

Expected runtime behavior:
- MQTT connection is not attempted;
- no fallback to `Config::MQTT_HOST`;
- no plaintext 1883 connection.

## D. Provisioning

POST valid HTTPS WebUI request:

```text
POST /api/mqtt/provision
host=<broker>
port=8883
user=<user>
pass=<password>
```

Expected HTTP `200`:

```json
{"ok":true,"provisioned":true}
```

Then:

```text
GET /api/mqtt/status
```

Expected:
- `provisioned:true`
- `connected:true` after the normal MQTT task connects
- password is absent from all JSON responses

## E. CSRF enforcement

Send provisioning POST without `X-CSRF-Token`.

Expected:
- HTTP `403`
- NVS credentials unchanged
- no MQTT reconnect/provisioning action

Send with an invalid token.

Expected:
- HTTP `403`

## F. Production plaintext rejection

With production build:

```text
POST /api/mqtt/provision
port=1883
```

Expected:
- HTTP `400`
- `plaintext MQTT disabled in production`
- no credentials committed as provisioned.

## G. TLS verification

Use a broker certificate signed by the configured CA.

Expected:
- TLS handshake succeeds after system time is synchronized.

Use a broker certificate signed by an unknown CA.

Expected:
- `secure_.connect()` fails;
- PubSubClient MQTT handshake is not attempted;
- `/LOG/MQTT-AUTH.LOG` receives `CONNECT_FAIL`.

Use an expired/not-yet-valid certificate.

Expected:
- TLS verification fails once the system clock is synchronized.

## H. Authentication failure

Provision correct broker host but intentionally wrong MQTT password.

Expected:
- broker rejects MQTT authentication;
- log contains `AUTH_FAIL` with the MQTT return state;
- reconnect uses exponential backoff.

## I. Disconnect logging

Connect successfully, then disable Wi-Fi or make the broker unreachable.

Expected:
- `/LOG/MQTT-AUTH.LOG` contains `DISCONNECT`;
- subsequent connection failures are logged;
- firmware continues operating without blocking the main/BLE tasks.

## J. Audit log rotation

Make `/LOG/MQTT-AUTH.LOG` exceed 64 KiB.

Expected:
- current log is renamed to `/LOG/MQTT-AUTH.1.LOG`;
- a new `/LOG/MQTT-AUTH.LOG` is created;
- no active log exceeds the configured rotation threshold before the next
  write/rotation.

## K. Password rotation warning

Provision a password and verify:

```text
GET /api/mqtt/status
```

Expected initially:

```json
"passwordRotationWarning":false
```

After `pass_epoch` reaches 90 days:

```json
"passwordRotationWarning":true
```

If the stored epoch is zero, the API also reports the warning so an operator
cannot mistake an unknown age for a fresh credential.

## L. Credential confidentiality

Inspect NVS using the normal diagnostic tooling.

Expected:
- no plaintext `host`, `user`, or `pass` keys are used by the new credential
  path;
- credential material exists only as the encrypted/authenticated `blob`;
- the password does not appear in MQTT audit logs.

## M. Regression

Run:

```bash
pio test -e native
pio run -e esp32-s3-wroom-1
```

Expected: existing tests/build continue to pass.

For the actual production build, repeat with:

```text
-DFIELDRADIO_PRODUCTION_BUILD
```

## Important security note

The application-level credential envelope derives its key from the device MAC.
It protects against accidental/plain NVS exposure but is not intended to resist
an attacker who can extract firmware and device identity. ESP32 Flash Encryption
and Secure Boot remain the production root-of-trust and are intentionally
handled in the later manufacturing phase.
