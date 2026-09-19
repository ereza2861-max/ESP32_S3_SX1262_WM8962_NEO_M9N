# FieldRadio dependency/version audit

| Component | Source | Current | Target (pinned) | Notes |
|---|---|---|---|---|
| platformio espressif32 | `platformio.ini` | `6.13.0` | `6.13.0` | Exact platform already present. |
| ESP-IDF | PlatformIO package | not explicitly pinned in repo | `# TODO(pin): confirm with upstream release notes` | Platform release selects IDF; do not infer a numeric IDF version here. |
| Arduino-ESP32 core | PlatformIO package | transitive | `# TODO(pin): confirm with upstream release notes` | Platform release controls the framework package. |
| NimBLE-Arduino | PlatformIO Registry | `^2.5.1` | `# TODO(pin): exact version` | Existing compatible range retained; `# audit: version not exact-pinned`. |
| PubSubClient | PlatformIO Registry | `^2.8` | `# TODO(pin): exact version` | Existing range retained. |
| RadioLib | PlatformIO Registry | `^7.7.1` | `# TODO(pin): exact version` | Existing range retained. |
| TinyGPSPlus | PlatformIO Registry | unpinned | `# TODO(pin): confirm with upstream release notes` | Existing dependency retained to avoid guessing. |
| Adafruit BME280 | PlatformIO Registry | `^2.2.4` (C3) | `# TODO(pin): exact version` | Existing range retained. |
| Adafruit NeoPixel | PlatformIO Registry | unpinned | `# TODO(pin): confirm with upstream release notes` | Existing dependency retained. |
| ESPWebServerSecure | Git URL | git HEAD | `# TODO(pin): confirm with upstream release notes` | No immutable tag/commit in current repo. |
| esp32-camera | PlatformIO/IDF | not present | `N/A` | No dependency found in current repository. |
| mbedtls | ESP-IDF component | transitive | `# TODO(pin): confirm with upstream release notes` | IDF-selected component. |
| esp-audio-simple-dec | ESP-IDF component | not present | `N/A` | No dependency found in current manifest. |
| esp-sr (AEC) | ESP-IDF component | `^2.5.2` | `# TODO(pin): exact version` | Existing range retained. |
| usb_device_uac | ESP-IDF component | `1.3.1` | `1.3.1` | Exact version already pinned. |
| zlib | ESP-IDF bundled component | transitive | `# TODO(pin): confirm with upstream release notes` | IDF-selected bundled version. |

`# audit:` entries intentionally preserve existing dependency constraints where the exact upstream version cannot be established from the repository alone.
