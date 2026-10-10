# Native HTTPS migration (ESP-IDF `esp_https_server`)

## Scope

This repository replaces the `esp32_idf5_https_server_compat` / `ESPWebServerSecure`
transport shim with native ESP-IDF HTTPS server APIs. The migration uses:

- `HttpdServer`: TLS listener, endpoint registration and callback trampoline.
- `HttpdRequest`: query/header/body parsing and remote address access.
- `HttpdResponse`: bounded response helpers and chunked file streaming.
- `HttpdMultipart`: streaming multipart parser used by WAV upload.
- `WebUi.h` / `WebUi.cpp`: request/response handler signatures and endpoint registration.
- `main.cpp`, `platformio.ini`, `sdkconfig.defaults`: native server selection and HTTPS component configuration.

The endpoint method/URI set was compared with the original repository source: all 121
URI+HTTP-method registrations are represented in the migrated `WebUi.cpp`.

## TLS material

`tools/provision-web-tls.py` generates `include/generated/WebTlsProvisioning.h` from
`secrets/web_tls_cert.der` and `secrets/web_tls_key.der`. Those secret files are
intentionally not included in this archive. Run the repository's documented provisioning
workflow before building a production image. The generated certificate is device-local;
clients must explicitly trust or provision its certificate/CA rather than disabling
certificate validation in production.

## Important behavior

- `HttpdServer` retains URI storage and handler objects for the lifetime of the server.
- The native server raises `max_uri_handlers` to 160 to accommodate the WebUI endpoint set.
- URL-encoded request bodies are limited by `HttpdRequest`; multipart upload is streamed
  with a configured maximum rather than buffered in RAM.
- File downloads use HTTP chunked transfer; `Content-Length` is not set alongside chunks.
- If any URI fails to register, the native HTTPS server is stopped and startup reports failure.

## Verification status

Static checks confirm the expected files exist, the legacy compatibility dependency and
`ESPWebServerSecure` references are removed from `src/`, and all 121 original URI/method
pairs are represented. A PlatformIO build and hardware HTTPS/upload/download tests were
not run in this environment because the `pio` executable and device are unavailable.
Run `pio run -e esp32-s3-wroom-1`, then exercise login/session/CSRF, multipart WAV upload,
and large file download on hardware before treating this migration as validated.
