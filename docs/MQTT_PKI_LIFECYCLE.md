# MQTT PKI Certificate Lifecycle

## Architecture

D-06 uses a vendor-neutral RFC 7030 EST endpoint or a backend adapter. The
firmware knows only `estServerUrl` and `estLabel`; the MQTT broker remains the
mTLS relying party. A deployment may place an adapter in front of a managed CA
when the broker itself does not expose RFC 7030.

The selected bootstrap flow is a factory bootstrap certificate. The device
starts with a device-unique certificate/private key, uses that certificate for
EST HTTPS mTLS, and never requires MQTT username/password authentication.

```text
Factory
  |
  | bootstrap X.509 + private key
  v
FieldRadio ---- HTTPS/mTLS ----> EST endpoint/adapter ----> CA/PKI
   |                                  |
   | <------ PKCS#7 certificate ------|
   |
   +---- MQTT mTLS ------------------> Broker
```

## Lifecycle

1. Parse the current certificate and read `notBefore` / `notAfter`.
2. Do nothing until GNSS UTC time is valid.
3. When `notAfter - now` is at or below `certRenewalThresholdDays`, generate a
   fresh P-256 private key.
4. Generate a PKCS#10 CSR with `CN=<device-id>`.
5. POST the CSR to `simpleenroll` for bootstrap-compatible enrollment or
   `simplereenroll` for renewal.
6. Parse the `application/pkcs7-mime` response and extract the issued X.509
   certificate.
7. Verify the certificate identity and X.509 chain.
8. Write certificate/key to the inactive NVS A/B slot, verify the bytes, then
   write the slot commit marker.
9. Reload the MQTT TLS transport and reconnect.
10. Record `ENROLLED`, `RENEWED`, `RENEW_FAILED`, `EXPIRED`, `VALIDATED`, or
    `REVOKED_WARNING` in `/LOG/CERT-LIFECYCLE.LOG`.

Password rotation is not part of the lifecycle.

## Runtime configuration

- `estServerUrl`: HTTPS EST endpoint or backend adapter.
- `estLabel`: RFC 7030 label, default `/.well-known/est`.
- `certRenewalThresholdDays`: default `30`.
- `certCheckPeriodMs`: default `86400000`.
- `estAuthMode`: `0` factory/previous client certificate, `1` basic auth,
  `2` bootstrap token. The locked deployment uses `0`.
- `certLifecycleEnabled`: default `false`.

All configuration changes go through the existing config snapshot/commit
pipeline. `CONFIG_VERSION=10` is paired with `ATOMIC_CONFIG_SCHEMA=2`.

## WebUI and serial console

Authenticated endpoints:

- `GET /api/mqtt/cert-status`
- `POST /api/mqtt/cert-renew`
- `GET /api/mqtt/cert-history`
- `POST /api/mqtt/cert-cacerts`

Serial commands require the existing challenge/response authentication:

- `cert status`
- `cert renew`
- `cert history`
- `est cacerts`
- `est csrattrs`

The `csrattrs` command is informational in this implementation because the
selected bootstrap profile does not require dynamic CSR attributes.

## Trust model

The current firmware reuses the generated `MQTT_BROKER_ROOT_CA` trust anchor
for EST TLS and issued-certificate verification. A deployment using a distinct
EST CA must provision an appropriate generated trust anchor before enabling the
lifecycle. The firmware must not silently disable TLS verification.

## Example step-ca / lab adapter

The exact URL exposed to the device must implement RFC 7030. If a PKI product
does not natively expose EST, deploy an EST adapter rather than pointing the
device directly at the MQTT broker.

A lab CA can be created with OpenSSL:

```sh
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 3650 \
  -subj "/CN=FieldRadio Lab CA" -out ca.crt

openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out device.key
openssl req -new -key device.key -subj "/CN=ESP32S3_VOICE_NODE_01" -out device.csr
```

The EST server/adapter is responsible for applying its CA policy and returning
a standards-compliant PKCS#7 certificate response.

## Operational requirements

- Never enable lifecycle before the factory bootstrap certificate is present.
- Do not print private keys or certificate material to the serial console.
- Keep EST endpoint credentials and CA private keys outside Git.
- Test renewal with an intentionally short-lived certificate before production.
- Test power loss after writing the inactive slot but before the commit marker.
- Test power loss after the commit marker and before MQTT reconnect.
- Revoke lost/stolen device certificates at the CA/broker side.
