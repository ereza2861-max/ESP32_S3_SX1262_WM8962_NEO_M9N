#pragma once

// Copy this file to include/LocalConfig.h for a local deployment.
// NEVER commit LocalConfig.h; it is ignored by .gitignore.
#define FIELDRADIO_AP_SSID "FieldRadio"
#define FIELDRADIO_AP_PASSWORD "replace-with-a-unique-password"
#define FIELDRADIO_WEB_USER "admin"
#define FIELDRADIO_WEB_PASSWORD "replace-with-a-different-password"
// 32-byte AES-128 key represented as 32 hexadecimal characters.
// Replace before deployment; never commit the real key.
#define FIELDRADIO_LORA_KEY_HEX "replace-with-32-hex-chars"
