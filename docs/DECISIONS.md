# Decisions

## D-06 — EST certificate lifecycle

**Status: CLOSED — Option C, PKI-based certificate lifecycle using EST (RFC 7030).**

- **Q1 = 1A:** EST mode 1 (Basic Auth) and mode 2 (Bearer/bootstrap token) use
  TLS server verification plus the `Authorization` header. They do not present
  the MQTT/device client certificate to EST. Mode 0 uses the client certificate.
- **Q2 = 2A:** EST credentials are stored through ESP-IDF encrypted NVS. The
  application does not add a second encryption layer. Mode 2 bootstrap tokens
  are removed from NVS after first successful enrollment. Mode 1 passwords
  remain available while mode 1 is active because renewal uses the same mode.
- **Q3 = 3C:** The previous HIL document is merged into
  `test/hil/test_hil_nvs_powerloss.md`; the old filename is removed. PKI
  power-loss cases cover both sides of the certificate commit marker and the
  MQTT reconnect boundary.

### Security invariants

- EST always uses a CA trust anchor; TLS verification is never disabled.
- A custom EST CA is supplied through `secrets/est_ca.pem` and generated into
  an ignored header. Without that secret, the firmware uses the existing public
  MQTT trust-anchor fallback defined by the project.
- Credentials are never printed to serial logs.
- WebUI configuration changes require authentication and CSRF validation.
- Certificate renewal is rate-limited to one manual request per minute.
- Renewal is not triggered until GNSS UTC time is valid.
