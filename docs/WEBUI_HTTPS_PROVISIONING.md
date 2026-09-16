# WebUI HTTPS provisioning (Rev-C)

The WebUI now listens only on HTTPS port 443. It does **not** fall back to plaintext HTTP when TLS material is missing. The Arduino `WebServer` API is retained through the IDF5-compatible `ESPWebServerSecure` compatibility layer, which wraps the ESP32 HTTPS server.

## Provisioning

For a fresh clone, the supported local flow is:

```text
make provision
make check-provisioning
make build
make upload
```

`make provision` creates the ignored credential/TLS inputs and never adds them
to Git. It prompts for the device AP/WebUI credentials and LoRa key, and creates
a self-signed lab certificate with SANs for `fieldradio.local` and `192.168.4.1`.

To use a controlled CA certificate instead, install the device-specific matching
DER pair in the ignored `secrets/` directory and run `make check-provisioning`.

Place device-specific DER files in the ignored `secrets/` directory:

```text
secrets/web_tls_cert.der
secrets/web_tls_key.der
```

The PlatformIO pre-build script validates the pair and converts them into the ignored `include/generated/WebTlsProvisioning.h`. Never commit the private key or generated header.

Example certificate generation for a lab device (adjust SANs to the actual deployment address/name):

```bash
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 825 \
  -keyout web_tls_key.pem -out web_tls_cert.pem \
  -subj "/CN=fieldradio.local" \
  -addext "subjectAltName=DNS:fieldradio.local,IP:192.168.4.1"
openssl x509 -in web_tls_cert.pem -outform DER -out secrets/web_tls_cert.der
openssl pkey -in web_tls_key.pem -outform DER -out secrets/web_tls_key.der
shred -u web_tls_key.pem 2>/dev/null || rm -f web_tls_key.pem
rm -f web_tls_cert.pem
```

For production, use a controlled CA and device-specific certificates rather than a self-signed certificate. Browsers will otherwise display a trust warning even though the TLS channel is encrypted.

## Security behavior

- Basic Auth credentials are now transported inside TLS.
- Session cookies are marked `Secure; HttpOnly; SameSite=Strict`.
- CSRF same-origin validation expects `https://<AP-IP>`.
- If certificate/key provisioning is missing, the WebUI is disabled rather than silently exposing HTTP.
- `collectHeaders()` is not called because the compatibility layer documents it as unimplemented; direct `header()` access is used instead.

The compatibility layer is an IDF5-oriented fork because the older compatibility package is known to be problematic with newer ESP-IDF releases.
