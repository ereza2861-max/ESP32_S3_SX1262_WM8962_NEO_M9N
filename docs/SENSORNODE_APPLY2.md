# FASE 1 — Apply WebUI BLE Sensor Inventory

From the repository root:

```bash
git apply PATCH.diff
git diff --check
```

This phase changes only:
- `include/SensorReader.h`
- `include/SensorRegistry.h`
- `include/WebUi.h`
- `src/SensorReader.cpp`
- `src/SensorRegistry.cpp`
- `src/WebUi.cpp`
- `test/test_sensor_registry.cpp`

No new repository source files are required for FASE 1.

The audited repository already contains `-I../shared` in the gateway `platformio.ini`, so no additional include-path edit is needed.

Build and test:

```bash
pio run -e esp32-s3-wroom-1
pio test -e native
```

New endpoints:

```text
GET  /api/sensors/nodes
GET  /api/sensors/nodes?id=N
GET  /api/sensors/live
POST /api/sensors/forget?id=N
POST /api/sensors/refresh?id=N
```

POST requests remain protected by the existing `WebUi::auth()` authentication, origin, and CSRF checks. Rate limiting is applied by the new handlers.

Implementation notes:
- WebUI does not manipulate NimBLE clients directly.
- `forget` and `refresh` are queued and consumed by the BLE task.
- Registry mutations and snapshots use a recursive mutex on Arduino; native tests use `std::recursive_mutex`.
- WebUI serializes only copied snapshots, so JSON generation does not hold the registry mutex.
- `lastSeenMs` retains the existing registry meaning: gateway `millis()`, not Unix epoch.
- Refresh and Reconnect intentionally invoke the same forced reconnect/re-discovery operation in this phase because the requested GAP-1 refresh endpoint is defined as forced reconnect.

The audit environment did not have the `pio` executable, so a full ESP32-S3 build could not be run here. Native `SensorRegistry` regression testing and patch application checks were run successfully.
