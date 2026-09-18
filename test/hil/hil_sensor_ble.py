from __future__ import annotations

import asyncio
import json
import os
import subprocess
import time
from dataclasses import dataclass

import pytest
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000001"
REQUEST_UUID = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000002"
DESCRIPTOR_UUID = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000003"
VALUE_UUID = "7f2a0000-7b2a-4a6e-9a9f-1b7f7e000004"
PROTOCOL_VERSION = 1
DESCRIPTOR_REQUEST_LIST = 0x01
DESCRIPTOR_RESPONSE_BYTES = 67
SENSOR_VALUE_BYTES = 15

NODE_NAME = os.getenv("FIELDRADIO_SENSOR_NAME", "FieldRadio-Sensor-C3")
NODE_ADDRESS = os.getenv("FIELDRADIO_SENSOR_ADDRESS", "").strip()
PAIR_PASSKEY = os.getenv("FIELDRADIO_SENSOR_PASSKEY", "").strip()
EVICTION_MS = int(os.getenv("FIELDRADIO_SENSOR_NODE_EVICTION_MS", "600000"))
MQTT_ROOT = os.getenv("FIELDRADIO_MQTT_TOPIC_ROOT", "fieldradio")
GATEWAY_DEVICE_ID = os.getenv("FIELDRADIO_GATEWAY_DEVICE_ID", "").strip()


def run(coro):
    return asyncio.run(coro)


async def find_node():
    devices = await BleakScanner.discover(timeout=float(os.getenv("FIELDRADIO_BLE_SCAN_TIMEOUT", "10")))
    for device in devices:
        uuids = [u.lower() for u in (device.metadata.get("uuids") or [])]
        if SERVICE_UUID.lower() in uuids:
            if NODE_ADDRESS and device.address.lower() != NODE_ADDRESS.lower():
                continue
            if NODE_NAME and device.name and device.name != NODE_NAME:
                continue
            return device
    return None


def require_node():
    device = run(find_node())
    assert device is not None, (
        f"BLE sensor node not found: name={NODE_NAME!r}, address={NODE_ADDRESS or '<any>'}, "
        f"service={SERVICE_UUID}. Power the ESP32-C3 on, advertise the service, "
        f"and check the wiring/firmware."
    )
    return device


def descriptor_request(index: int) -> bytes:
    return bytes((PROTOCOL_VERSION, DESCRIPTOR_REQUEST_LIST, index))


def parse_descriptor(raw: bytes) -> dict:
    assert len(raw) == DESCRIPTOR_RESPONSE_BYTES, (
        f"Descriptor response length is {len(raw)}, expected {DESCRIPTOR_RESPONSE_BYTES}"
    )
    magic = int.from_bytes(raw[0:2], "little")
    version = raw[2]
    index = raw[3]
    total = raw[4]
    # SensorDescriptor is packed: id,type,name[24],unit[12],datatype,
    # scale,offset,min,max,periodMs,flags.
    sid = int.from_bytes(raw[5:7], "little")
    dtype = raw[7]
    name = raw[8:32].split(b"\0", 1)[0].decode(errors="replace")
    unit = raw[32:44].split(b"\0", 1)[0].decode(errors="replace")
    datatype = raw[44]
    period_ms = int.from_bytes(raw[61:65], "little")
    flags = int.from_bytes(raw[65:67], "little")
    return {
        "magic": magic, "version": version, "index": index, "total": total,
        "id": sid, "type": dtype, "name": name, "unit": unit,
        "datatype": datatype, "periodMs": period_ms, "flags": flags,
    }


async def discover_descriptors(client: BleakClient) -> list[dict]:
    chars = client.services.get_characteristic(DESCRIPTOR_UUID)
    request = client.services.get_characteristic(REQUEST_UUID)
    assert request is not None, f"Missing descriptor request characteristic {REQUEST_UUID}"
    assert chars is not None, f"Missing descriptor data characteristic {DESCRIPTOR_UUID}"

    descriptors: list[dict] = []
    # First request determines total.
    await client.write_gatt_char(REQUEST_UUID, descriptor_request(0), response=True)
    first = parse_descriptor(await client.read_gatt_char(DESCRIPTOR_UUID))
    assert first["magic"] == 0x5344, f"Bad descriptor magic: {first['magic']:#x}"
    assert first["version"] == PROTOCOL_VERSION, f"Unsupported protocol version {first['version']}"
    assert first["index"] == 0
    assert 1 <= first["total"] <= 16
    descriptors.append(first)

    for index in range(1, first["total"]):
        await client.write_gatt_char(REQUEST_UUID, descriptor_request(index), response=True)
        item = parse_descriptor(await client.read_gatt_char(DESCRIPTOR_UUID))
        assert item["index"] == index
        assert item["total"] == first["total"]
        assert item["id"] != 0
        assert item["name"]
        descriptors.append(item)
    return descriptors


async def connect_for_test(address: str):
    client = BleakClient(address)
    ok = await client.connect()
    assert ok or client.is_connected, f"BLE connect failed for {address}"
    return client


@pytest.mark.hil
def test_pairing_passkey(pair_command):
    """Pair using a real OS/platform pairing helper.

    Bleak exposes pairing but passkey entry is delegated to the host Bluetooth
    stack on several platforms. FIELDRADIO_BLE_PAIR_COMMAND therefore provides
    the actual passkey injection mechanism for the CI/lab host.
    """
    device = require_node()
    assert PAIR_PASSKEY.isdigit() and len(PAIR_PASSKEY) == 6, (
        "Set FIELDRADIO_SENSOR_PASSKEY to the node's six-digit provisioning passkey."
    )
    assert pair_command, (
        "Set FIELDRADIO_BLE_PAIR_COMMAND to a host pairing command that accepts "
        "{address} and {passkey}. Bleak cannot inject a numeric passkey uniformly "
        "across Windows/macOS/Linux Bluetooth backends."
    )
    command = pair_command.format(address=device.address, passkey=PAIR_PASSKEY)
    result = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=60)
    assert result.returncode == 0, (
        f"Correct-passkey pairing failed (rc={result.returncode}). "
        f"stdout={result.stdout[-1000:]!r} stderr={result.stderr[-1000:]!r}"
    )
    async def verify():
        client = await connect_for_test(device.address)
        try:
            assert client.is_connected
            assert client.services.get_service(SERVICE_UUID) is not None
        finally:
            await client.disconnect()
    run(verify())


@pytest.mark.hil
def test_pairing_wrong_passkey(pair_command):
    device = require_node()
    assert PAIR_PASSKEY.isdigit() and len(PAIR_PASSKEY) == 6, "Set FIELDRADIO_SENSOR_PASSKEY."
    assert pair_command, (
        "Set FIELDRADIO_BLE_PAIR_COMMAND. The command must perform an actual pairing "
        "attempt using {address} and {passkey}; do not replace this with a mock."
    )
    wrong = "000000" if PAIR_PASSKEY != "000000" else "999999"
    command = pair_command.format(address=device.address, passkey=wrong)
    failures = 0
    for _ in range(3):
        result = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=60)
        if result.returncode != 0:
            failures += 1
    assert failures == 3, (
        f"Wrong-passkey pairing unexpectedly succeeded {3-failures} time(s). "
        "This test expects the node to reject all three attempts."
    )
    # A fourth attempt during the block interval must also fail.
    result = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=30)
    assert result.returncode != 0, (
        "Fourth wrong-passkey attempt succeeded immediately; expected the "
        "60-second pairing block after three failures."
    )


@pytest.mark.hil
def test_reconnect_after_drop():
    device = require_node()
    async def scenario():
        client = await connect_for_test(device.address)
        await client.disconnect()
        await asyncio.sleep(float(os.getenv("FIELDRADIO_RECONNECT_WAIT", "2")))
        client2 = await connect_for_test(device.address)
        try:
            assert client2.is_connected
        finally:
            await client2.disconnect()
    run(scenario())


@pytest.mark.hil
def test_sensor_discovery():
    device = require_node()
    async def scenario():
        client = await connect_for_test(device.address)
        try:
            descriptors = await discover_descriptors(client)
            assert descriptors, "Gateway contract requires at least one descriptor."
            ids = [d["id"] for d in descriptors]
            assert len(ids) == len(set(ids)), f"Duplicate sensor IDs discovered: {ids}"
        finally:
            await client.disconnect()
    run(scenario())


@pytest.mark.hil
def test_node_loss(gateway_http):
    """Power the node off and verify gateway eviction.

    The test records the node ID from inventory, powers the node down through
    FIELDRADIO_SENSOR_POWER_OFF_COMMAND, then waits longer than the configured
    eviction period. This is an intentional physical/relay control point.
    """
    power_off = os.getenv("FIELDRADIO_SENSOR_POWER_OFF_COMMAND", "").strip()
    assert power_off, (
        "Set FIELDRADIO_SENSOR_POWER_OFF_COMMAND to the lab relay/power-control "
        "command accepting {address}. The test must actually remove node power."
    )
    nodes = gateway_http.get(
        f"{gateway_http.base_url}/api/sensors/nodes", timeout=10
    ).json().get("nodes", [])
    target = next(
        (n for n in nodes if n.get("address", "").lower() == NODE_ADDRESS.lower()),
        None,
    ) if NODE_ADDRESS else None
    assert target, "Gateway inventory does not contain the configured sensor node."
    node_id = target["id"]
    result = subprocess.run(
        power_off.format(address=NODE_ADDRESS),
        shell=True, text=True, capture_output=True, timeout=30
    )
    assert result.returncode == 0, f"Sensor power-off command failed: {result.stderr[-500:]}"
    deadline = time.monotonic() + (EVICTION_MS / 1000.0) + 30
    while time.monotonic() < deadline:
        data = gateway_http.get(
            f"{gateway_http.base_url}/api/sensors/nodes", timeout=10
        ).json()
        if not any(n.get("id") == node_id for n in data.get("nodes", [])):
            return
        time.sleep(5)
    pytest.fail(
        f"Node id={node_id} remained in gateway inventory after "
        f"{EVICTION_MS} ms + 30 s grace."
    )


@pytest.mark.hil
def test_sensor_count_change(sensor_node_serial, gateway_http):
    """Change 5 -> 3 -> 5 sensors and verify descriptor refresh."""
    address = NODE_ADDRESS.lower()
    assert address, "Set FIELDRADIO_SENSOR_ADDRESS to select the sensor node."
    # Stage the count change; the node's command handler updates the live registry.
    from conftest import serial_command
    for count in (3, 5):
        lines = serial_command(sensor_node_serial, f"sensors {count}")
        assert any("OK: sensor count staged" in x for x in lines), (
            f"Sensor node rejected 'sensors {count}': {lines}"
        )
        time.sleep(1)
        data = gateway_http.get(
            f"{gateway_http.base_url}/api/sensors/nodes", timeout=10
        ).json()
        node = next((n for n in data.get("nodes", [])
                     if n.get("address", "").lower() == address), None)
        assert node is not None, "Gateway lost sensor node during count-change test."
        # Force the gateway descriptor refresh through its actual endpoint.
        token = gateway_http.headers["X-CSRF-Token"]
        resp = gateway_http.post(
            f"{gateway_http.base_url}/api/sensors/refresh?id={node['id']}",
            headers={"X-CSRF-Token": token, "Origin": gateway_http.base_url},
            timeout=10,
        )
        assert resp.status_code == 202, f"Refresh request failed: {resp.status_code} {resp.text}"
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            live = gateway_http.get(
                f"{gateway_http.base_url}/api/sensors/nodes?id={node['id']}", timeout=10
            ).json()
            if live.get("ok") and len(live.get("sensors", [])) == count:
                break
            time.sleep(1)
        else:
            pytest.fail(f"Gateway did not rediscover exactly {count} descriptors.")


@pytest.mark.hil
def test_value_notification(mqtt_broker):
    """Subscribe to the production sensor topic and require a real notification."""
    import paho.mqtt.client as mqtt

    assert GATEWAY_DEVICE_ID, "Set FIELDRADIO_GATEWAY_DEVICE_ID for the MQTT topic."
    assert NODE_ADDRESS, "Set FIELDRADIO_SENSOR_ADDRESS."
    received: list[dict] = []
    topic_prefix = f"{MQTT_ROOT}/{GATEWAY_DEVICE_ID}/sensor/"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    def on_connect(c, userdata, flags, reason_code, properties):
        assert reason_code == 0, f"MQTT connect failed: {reason_code}"
        c.subscribe(topic_prefix + "#", qos=0)

    def on_message(c, userdata, msg):
        try:
            received.append(json.loads(msg.payload.decode()))
        except Exception:
            pass

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(mqtt_broker["host"], mqtt_broker["port"], keepalive=30)
    client.loop_start()
    try:
        deadline = time.monotonic() + mqtt_broker["timeout"]
        while time.monotonic() < deadline and not received:
            time.sleep(0.2)
        assert received, (
            f"No sensor MQTT notification received under {topic_prefix} within "
            f"{mqtt_broker['timeout']} s. Verify BLE notifications, gateway "
            "sensor queue, MQTT credentials, and broker configuration."
        )
        sample = received[-1]
        for key in ("ts", "node", "sensor", "sensor_name", "unit", "value", "quality", "rssi"):
            assert key in sample, f"MQTT sensor payload missing {key}: {sample}"
    finally:
        client.loop_stop()
        client.disconnect()
