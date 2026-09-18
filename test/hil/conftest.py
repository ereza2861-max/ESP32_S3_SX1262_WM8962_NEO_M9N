"""Hardware-in-the-loop fixtures for FieldRadio.

These tests deliberately fail with actionable messages when the required
hardware/control path is not configured. They never silently skip a required
HIL test.
"""
from __future__ import annotations

import os
import socket
import subprocess
import time
from pathlib import Path

import pytest

try:
    import serial
except ImportError:
    serial = None

try:
    import requests
except ImportError:
    requests = None


def _env(name: str, *, secret: bool = False) -> str:
    value = os.getenv(name, "").strip()
    if not value:
        hint = "<set>" if secret else "a non-empty value"
        raise AssertionError(
            f"HIL prerequisite missing: set {name}={hint}. "
            f"See test/hil/README.md."
        )
    return value


def _int_env(name: str, default: int) -> int:
    raw = os.getenv(name, str(default))
    try:
        return int(raw, 10)
    except ValueError as exc:
        raise AssertionError(f"{name} must be an integer, got {raw!r}") from exc


@pytest.fixture(scope="session")
def gateway_serial():
    if serial is None:
        raise AssertionError("pyserial is required: pip install -r test/hil/requirements.txt")
    port = _env("FIELDRADIO_GATEWAY_SERIAL")
    baud = _int_env("FIELDRADIO_GATEWAY_BAUD", 115200)
    with serial.Serial(port, baudrate=baud, timeout=0.2) as ser:
        yield ser


@pytest.fixture(scope="session")
def sensor_node_serial():
    if serial is None:
        raise AssertionError("pyserial is required: pip install -r test/hil/requirements.txt")
    port = _env("FIELDRADIO_SENSOR_SERIAL")
    baud = _int_env("FIELDRADIO_SENSOR_BAUD", 115200)
    with serial.Serial(port, baudrate=baud, timeout=0.2) as ser:
        yield ser


@pytest.fixture(scope="session")
def gateway_url():
    return _env("FIELDRADIO_GATEWAY_URL").rstrip("/")


@pytest.fixture(scope="session")
def gateway_http(gateway_url):
    if requests is None:
        raise AssertionError("requests is required: pip install -r test/hil/requirements.txt")
    user = _env("FIELDRADIO_WEB_USER")
    password = _env("FIELDRADIO_WEB_PASSWORD", secret=True)
    verify = os.getenv("FIELDRADIO_TLS_VERIFY", "0").lower() in {"1", "true", "yes"}
    session = requests.Session()
    session.auth = (user, password)
    session.verify = verify
    session.headers.update({"User-Agent": "FieldRadio-HIL/2"})
    # Establish the authenticated session before tests use CSRF-protected POSTs.
    response = session.get(f"{gateway_url}/api/status", timeout=10)
    if response.status_code != 200:
        raise AssertionError(
            f"Gateway authentication/status failed: HTTP {response.status_code}: "
            f"{response.text[:300]}"
        )
    csrf = session.get(f"{gateway_url}/api/v1/csrf", timeout=10)
    if csrf.status_code != 200:
        raise AssertionError(
            f"Gateway CSRF endpoint failed: HTTP {csrf.status_code}: {csrf.text[:300]}"
        )
    try:
        token = csrf.json()["token"]
    except Exception as exc:
        raise AssertionError("Gateway CSRF response has no JSON token") from exc
    session.headers.update({"X-CSRF-Token": token})
    session.headers.update({"Origin": gateway_url})
    session.base_url = gateway_url
    yield session


@pytest.fixture(scope="session")
def mqtt_config():
    host = os.getenv("FIELDRADIO_MQTT_HOST", "127.0.0.1")
    port = _int_env("FIELDRADIO_MQTT_PORT", 1883)
    timeout = float(os.getenv("FIELDRADIO_MQTT_TIMEOUT", "15"))
    return {"host": host, "port": port, "timeout": timeout}


@pytest.fixture(scope="session")
def mqtt_broker(mqtt_config):
    """Verify the configured local MQTT broker is actually reachable.

    This fixture does not start a hidden broker: the README explicitly requires
    mosquitto. If the broker is absent, the test fails instead of being skipped.
    """
    try:
        with socket.create_connection(
            (mqtt_config["host"], mqtt_config["port"]), timeout=3
        ):
            pass
    except OSError as exc:
        raise AssertionError(
            f"MQTT broker is not reachable at "
            f"{mqtt_config['host']}:{mqtt_config['port']}: {exc}. "
            f"Start mosquitto or set FIELDRADIO_MQTT_HOST/PORT."
        ) from exc
    return mqtt_config


def serial_command(ser, command: str, timeout: float = 3.0) -> list[str]:
    ser.reset_input_buffer()
    ser.write((command.rstrip("\n") + "\n").encode())
    ser.flush()
    deadline = time.monotonic() + timeout
    lines: list[str] = []
    while time.monotonic() < deadline:
        raw = ser.readline()
        if raw:
            line = raw.decode(errors="replace").strip()
            lines.append(line)
            if line.startswith(("OK:", "ERROR:", "FATAL:", "Provisioning saved")):
                break
    return lines


@pytest.fixture(scope="session")
def lora_injector():
    command = os.getenv("FIELDRADIO_LORA_INJECT_COMMAND", "").strip()
    if not command:
        return None
    return command


@pytest.fixture(scope="session")
def pair_command():
    return os.getenv("FIELDRADIO_BLE_PAIR_COMMAND", "").strip() or None
