# Audit notes — 2026-10-02

- The supplied audit report refers to `test/native/test_sensor_registry.cpp` and
  `test/native/test_sensor_queue_policy.cpp`, but this repository stores those
  operational native tests under `test/test_sensor_registry.cpp` and
  `test/test_sensor_queue_policy.cpp`. Those existing files were updated in place;
  no tests were deleted or moved.
- The native PlatformIO environment does not expose Arduino SD/File primitives,
  so the new `test/native/test_sensor_spool_metadata.cpp` and
  `test/native/test_mqtt_sensor_payload.cpp` are host contract tests rather than
  direct hardware/SD integration tests. The production implementation remains
  covered by the target firmware build path and should receive hardware SD HIL
  evidence separately.
- Historical material under `docs/archive/` and the audit report itself still
  contains the old 67/15 wording intentionally; these are retained as historical
  evidence rather than rewritten operational documentation.
- The audit's other decision-required items (MQTT durability policy, calibration
  lifecycle, placeholder drivers, PCB fabrication evidence, and production HIL
  gates) were not changed because D1–D10 do not freeze decisions for them.
