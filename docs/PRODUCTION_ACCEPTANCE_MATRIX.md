# Production acceptance matrix

This matrix is an evidence gate, not a substitute for manufacturing or HIL
execution. A source/build result cannot claim physical production acceptance.

| Evidence | Required artifact | Acceptance check |
| --- | --- | --- |
| Build reproducibility | `artifacts/production/build.bin` + `build.sha256` | SHA-256 manifest matches the artifact |
| eFuse state | `artifacts/production/efuse-summary.txt` | Secure Boot and Flash Encryption are explicitly enabled |
| Device certificate | `artifacts/production/device-cert.pem` | File is present and non-empty |
| Manufacturing evidence | existing signing/provisioning/HIL reports | Reviewed separately by manufacturing |

The acceptance script intentionally fails closed when any required artifact or
security-state line is absent. It does not burn eFuses, sign firmware, or
manufacture certificates.

## C3/S3 sensor production gates
Each profile requires four independent gates before production enablement: (1) electrical validation, (2) sensor protocol validation, (3) calibration validation/traceability, and (4) HIL/recovery validation. Source/build success alone does not set `PRODUCTION_READY`.


## Explicit release gates — audit response

- Hardware/PCB: **NOT VALIDATED**. PCB is not assumed fabricated; pin map remains
  candidate Rev-C. Do not claim production-ready based on source changes.
- HIL: **PENDING**. Run `test/hil/run_hil.py` with the planned attenuator and
  existing USB Serial/Wi-Fi paths; complete and sign `test/hil/CHECKLIST.md`.
- MQTT rotation: demonstrate EST issuance, old/new credential overlap, successful
  connection with the replacement, retirement of the prior credential, and audit
  events in `/LOG/MQTT-AUTH.LOG`.
- Sensor profiles: electrical, protocol, calibration and HIL gates must all pass;
  `PRODUCTION_READY[]` must remain false until then.
