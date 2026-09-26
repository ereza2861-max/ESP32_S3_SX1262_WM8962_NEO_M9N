# FASE 3 — GAP-3 Apply

## 1. Apply

From repository root:

```bash
git apply PATCH3.diff
git diff --check
```

The patch modifies only existing files:

```text
include/MqttClientManager.h
src/MqttClientManager.cpp
include/Config.h
src/WebUi.cpp
```

No private credential is included.

## 2. Production build flag

The existing gateway environment is unchanged by this phase. For a production
build, add this compiler definition to the production build configuration:

```text
-DFIELDRADIO_PRODUCTION_BUILD
```

Do not define it for the local/HIL environment unless you intentionally want
the production behavior.

With the flag defined:
- MQTT NVS credentials are mandatory.
- Missing/unprovisioned MQTT credentials prevent connection.
- port 1883 is rejected.
- compile-time MQTT host/user/password values cannot be used as a fallback.

The default MQTT host remains available for non-production development builds.

## 3. MQTT provisioning

After HTTPS WebUI authentication, obtain the CSRF token from the existing
`/api/v1/csrf` endpoint, then POST form fields:

```text
host
port
user
pass
```

Example:

```bash
curl -k -b cookies.txt   -H 'X-CSRF-Token: <token>'   -d 'host=mqtt.example.com&port=8883&user=gateway01&pass=<password>'   https://<gateway>/api/mqtt/provision
```

Expected:

```json
{"ok":true,"provisioned":true}
```

The password is never returned by the API.

Credentials are stored as an authenticated encrypted application envelope in
the `mqtt_creds` NVS namespace. The encryption key is derived from the ESP32
Wi-Fi STA MAC and a firmware constant. Production Flash Encryption should still
be enabled; application encryption is defense-in-depth, not a replacement for
ESP32 flash encryption.

## 4. Status / 90-day warning

```bash
curl -k -b cookies.txt https://<gateway>/api/mqtt/status
```

Expected fields:

```text
provisioned
connected
passwordRotationWarning
```

`passwordRotationWarning=true` means the stored password is at least 90 days
old, or its provisioning time cannot be established.

## 5. TLS

TLS connections:
- load the configured broker CA;
- require a synchronized clock before MQTT connection;
- set a 10-second TLS handshake timeout;
- explicitly establish the secure transport before invoking PubSubClient MQTT
  connect.

The existing `WiFiClientSecure` CA verification remains enabled; the code does
not call `setInsecure()`.

## 6. Audit log

MQTT connection events are written to:

```text
/LOG/MQTT-AUTH.LOG
```

The active file is rotated at 64 KiB to:

```text
/LOG/MQTT-AUTH.1.LOG
```

Events include:
- `CONNECT_OK`
- `DISCONNECT`
- `AUTH_FAIL`
- `CONNECT_FAIL`
- `PROVISIONED`

The log contains the broker hostname and MQTT return state, but never the
username or password.

## 7. Current sensor-node configuration

The sensor node is green-field. Runtime profile selection uses the `sensor/profile`
NVS key and the WebUI. Each profile has an immutable source-level roster, and
only the selected profile is instantiated after reboot.
