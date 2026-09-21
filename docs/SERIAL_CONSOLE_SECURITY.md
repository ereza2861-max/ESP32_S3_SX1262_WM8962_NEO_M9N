# Production serial-console security

The production serial console uses authenticated challenge/response for every
command except `status`, `help`, and the authentication commands themselves.

## Protocol

1. Send `auth challenge`.
2. The device returns a 16-byte random challenge as 32 lowercase hexadecimal
   characters.
3. Derive the 32-byte authentication key as:

   `SHA-256(loraKeyHex || "FieldRadio-Serial-Console-v1")`

4. Send `auth <64-hex-hmac>`, where the response is
   `HMAC-SHA256(authentication_key, challenge)`.
5. A successful authentication opens a 60-second privileged session.

The response comparison is constant-time. The challenge and authentication
session are invalidated when the session expires or a new challenge is issued.

The authentication mechanism is intentionally tied to the existing protected
LoRa key material so the firmware does not introduce a second long-lived
production secret. Production provisioning must therefore protect the LoRa
key with the same controls used for the rest of the device key material.

Unauthenticated serial access remains able to retrieve basic health/status
output, but it cannot execute configuration, credential provisioning,
LoRaWAN control/uplink, peer management, reboot, wipe, or log commands.
