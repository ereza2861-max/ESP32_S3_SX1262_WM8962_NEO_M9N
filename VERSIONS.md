# FieldRadio dependency/version audit

| Component | Source | Current | Target (pinned) | Notes |
|---|---|---|---|---|
| platformio espressif32 | `platformio.ini` | `6.13.0` | `6.13.0` | Exact platform already present. |
| ESP-IDF | PlatformIO package | not explicitly pinned in repo | `4.4.7` | Selected by `espressif32 @ 6.13.0` + Arduino framework; cannot be independently pinned here. |
| Arduino-ESP32 core | PlatformIO package | transitive | `2.0.17` | Selected by the PlatformIO platform/framework combination; cannot be independently pinned here. |
| NimBLE-Arduino | PlatformIO Registry | `^2.5.1` | `2.5.1` | Exact version pinned in `lib_deps`. |
| PubSubClient | PlatformIO Registry | `^2.8` | `2.8` | Exact version pinned in `lib_deps`. |
| RadioLib | PlatformIO Registry | `^7.7.1` | `7.7.1` | Exact version pinned in `lib_deps`. |
| TinyGPSPlus | PlatformIO Registry | unpinned | `1.0.2` | Exact version pinned in `lib_deps`; registry metadata has a known issue, but the requested version is correct. |
| Adafruit BME280 | PlatformIO Registry | `^2.2.4` (C3) | `2.3.0` | Exact version pinned in `lib_deps`. |
| Adafruit NeoPixel | PlatformIO Registry | unpinned | `1.15.5` | Exact version pinned in `lib_deps`. |
| ESPWebServerSecure | Git URL | `jackjansen/esp32_idf5_https_server_compat` at unresolved placeholder | **UNRESOLVED** | The repository currently uses an IDF-5-oriented fork while PlatformIO 6.13.0 + Arduino resolves to the IDF-4.4.7 stack; choose the compatible build stack before pinning a commit. |
| esp32-camera | PlatformIO/IDF | not present | `N/A` | No dependency found in current repository. |
| mbedtls | ESP-IDF component | transitive | `2.28.x` | Bundled by ESP-IDF v4.4.7; cannot be independently pinned. |
| esp-audio-simple-dec | ESP-IDF component | not present | `N/A` | No dependency found in current manifest. |
| esp-sr (AEC) | ESP-IDF component | `^2.5.2` | `2.5.2` | Exact version pinned in `idf_component.yml`. |
| usb_device_uac | ESP-IDF component | `1.3.1` | `1.3.1` | Exact version already pinned. |
| zlib | ESP-IDF bundled component | transitive | `ESP-IDF v4.4.7 bundled` | Bundled by ESP-IDF v4.4.7; cannot be independently pinned. |

ESP-IDF v4.4.7 and Arduino-ESP32 core v2.0.17 are selected by the confirmed PlatformIO/framework combination rather than independently declared component versions.
