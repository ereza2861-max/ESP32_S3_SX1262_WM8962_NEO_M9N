# FASE 1 — Verification

## Inventory

```bash
curl -k -b cookies.txt https://<gateway>/api/sensors/nodes
```

Expected HTTP `200`, `ok:true`, and `nodes[]` entries containing `id`, `address`, `name`, `rssi`, `connected`, `lastSeenMs`, and `sensorCount`.

## Node detail

```bash
curl -k -b cookies.txt 'https://<gateway>/api/sensors/nodes?id=0'
```

Expected HTTP `200` for an existing node, with all descriptors and latest values. Each sensor includes `id`, `name`, `unit`, `valueValid`, `value`, `timestamp`, and `quality`.

A nonexistent node must return HTTP `404`.

## Live polling

```bash
curl -k -b cookies.txt https://<gateway>/api/sensors/live
```

Expected HTTP `200`, `ok:true`, with current node/sensor snapshots. The WebUI polls this endpoint every 2 seconds.

## CSRF

A POST without `X-CSRF-Token` must return HTTP `403` and must not trigger a BLE action.

With a valid authenticated session and CSRF token:

```bash
curl -k -b cookies.txt   -H 'X-CSRF-Token: <token>'   -X POST 'https://<gateway>/api/sensors/refresh?id=0'
```

Expected HTTP `202` and `queued:true`.

## Refresh / reconnect

After the refresh request:
1. The BLE task tears down the existing client.
2. The scanner finds the node again.
3. GATT descriptors are discovered again.
4. The inventory returns to `connected:true`.

## Forget

```bash
curl -k -b cookies.txt   -H 'X-CSRF-Token: <token>'   -X POST 'https://<gateway>/api/sensors/forget?id=0'
```

Expected HTTP `202`. The BLE task removes the node from the registry; the next inventory poll no longer shows it.

## UI

The FieldRadio page must contain a `BLE Sensor Nodes` card with:
- Node
- Address
- RSSI
- Sensors
- Last Seen
- Status
- Refresh / Reconnect / Forget actions

Clicking a node opens the descriptor/value table. Quality is rendered as a badge.

## Regression

```bash
pio test -e native
```

Expected all existing tests plus the new snapshot/forget regression to pass.

For runtime robustness, leave a BLE node sending notifications continuously while polling `/api/sensors/live` every 2 seconds and exercise reconnect/forget. There should be no watchdog reset, heap corruption, or BLE task blockage attributable to WebUI.

## Audit results

Verified in the audit environment:
- `git apply --check`: PASS
- `git apply`: PASS
- `git diff --check`: PASS
- native `SensorRegistry` regression executable: PASS

Not verified here:
- full ESP32-S3 PlatformIO build, because `pio` is not installed in the audit environment.
