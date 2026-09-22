#pragma once

// For a fresh device, prefer: make provision
// This template is retained for manual/automated provisioning.
// NEVER commit LocalConfig.h; it is ignored by .gitignore.
#define FIELDRADIO_AP_SSID "FieldRadio"
#define FIELDRADIO_AP_PASSWORD "replace-with-a-unique-password"
#define FIELDRADIO_WEB_USER "admin"
#define FIELDRADIO_WEB_PASSWORD "replace-with-a-different-password"
// 32-byte AES-128 key represented as 32 hexadecimal characters.
// Replace before deployment; never commit the real key.
#define FIELDRADIO_LORA_KEY_HEX "replace-with-32-hex-chars"

// HTTPS certificate provisioning is performed from ignored DER files:
//   secrets/web_tls_cert.der
//   secrets/web_tls_key.der
// The PlatformIO pre-build script converts them into an ignored generated header.

// Optional deployment overrides for STA/MQTT. Never commit LocalConfig.h.
// The supplied deployment password belongs here locally, not in Git.
#define FIELDRADIO_STA_SSID "replace-with-sta-ssid"
#define FIELDRADIO_STA_PASSWORD "replace-with-sta-password"
#define FIELDRADIO_DEVICE_ID "ESP32S3_VOICE_NODE_01"
#define FIELDRADIO_CALLSIGN "FIELD_RADIO_01"
#define FIELDRADIO_MQTT_HOST "broker.emqx.io"
#define FIELDRADIO_MQTT_PORT 8883
#define FIELDRADIO_MQTT_USERNAME ""
#define FIELDRADIO_MQTT_PASSWORD ""
#define FIELDRADIO_MQTT_TOPIC_ROOT "fieldradio"
#define FIELDRADIO_MQTT_SERVER_NAME "broker.emqx.io"

// EST / PKI lifecycle is disabled by default. Enable only after an RFC 7030
// endpoint/adapter and factory bootstrap certificate are provisioned.
#define FIELDRADIO_EST_SERVER_URL ""
#define FIELDRADIO_EST_LABEL "/.well-known/est"
#define FIELDRADIO_CERT_RENEWAL_THRESHOLD_DAYS 30
#define FIELDRADIO_CERT_CHECK_PERIOD_MS 86400000UL
#define FIELDRADIO_EST_AUTH_MODE 0
#define FIELDRADIO_CERT_LIFECYCLE_ENABLED 0
