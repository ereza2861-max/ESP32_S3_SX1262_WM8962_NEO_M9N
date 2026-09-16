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
