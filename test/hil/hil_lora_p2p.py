from __future__ import annotations

import json
import os
import subprocess
import time
import uuid

import pytest

MESSAGE_ENDPOINT = "/api/message"
MESSAGES_ENDPOINT = "/api/messages"
SOS_ENDPOINT = "/api/sos"
SOS_STATUS_ENDPOINT = "/api/sos-status"
LORA_INJECT_COMMAND = os.getenv("FIELDRADIO_LORA_INJECT_COMMAND", "").strip()


def _gateway_url(name: str) -> str:
    value = os.getenv(name, "").strip().rstrip("/")
    assert value, f"Set {name} for this LoRa HIL test."
    return value


def _session(base_url: str):
    import requests
    user = os.getenv("FIELDRADIO_WEB_USER", "")
    password = os.getenv("FIELDRADIO_WEB_PASSWORD", "")
    assert user and password, "Set FIELDRADIO_WEB_USER and FIELDRADIO_WEB_PASSWORD."
    verify = os.getenv("FIELDRADIO_TLS_VERIFY", "0").lower() in {"1", "true", "yes"}
    s = requests.Session()
    s.auth = (user, password)
    s.verify = verify
    s.headers.update({"Origin": base_url, "User-Agent": "FieldRadio-LoRa-HIL/2"})
    r = s.get(f"{base_url}/api/status", timeout=10)
    assert r.status_code == 200, f"{base_url}: authentication failed: {r.status_code}"
    c = s.get(f"{base_url}/api/v1/csrf", timeout=10)
    assert c.status_code == 200, f"{base_url}: CSRF endpoint failed: {c.status_code}"
    s.headers["X-CSRF-Token"] = c.json()["token"]
    return s


@pytest.fixture(scope="module")
def lora_gateways():
    a = _gateway_url("FIELDRADIO_GATEWAY_A_URL")
    b = _gateway_url("FIELDRADIO_GATEWAY_B_URL")
    return _session(a), _session(b)


def _messages(session):
    r = session.get(f"{session.base_url}/api/messages", timeout=10)
    assert r.status_code == 200, f"messages endpoint failed: {r.status_code} {r.text[:300]}"
    data = r.json()
    return data if isinstance(data, list) else data.get("messages", [])


def _send_text(session, text: str):
    r = session.post(
        f"{session.base_url}{MESSAGE_ENDPOINT}",
        params={"plain": text},
        timeout=15,
    )
    assert r.status_code == 200, f"LoRa text send failed: {r.status_code} {r.text[:500]}"
    return r.json()


@pytest.mark.hil
def test_encrypted_text_roundtrip(lora_gateways):
    a, b = lora_gateways
    marker = f"HIL-{uuid.uuid4().hex}"
    before = _messages(b)
    result = _send_text(a, marker)
    assert result.get("sent") is True, f"Gateway did not queue encrypted text: {result}"
    deadline = time.monotonic() + float(os.getenv("FIELDRADIO_LORA_HIL_TIMEOUT", "30"))
    while time.monotonic() < deadline:
        if any(marker in json.dumps(m) for m in _messages(b)):
            return
        time.sleep(1)
    pytest.fail("Encrypted LoRa text did not arrive at gateway B before timeout.")


@pytest.mark.hil
def test_replay_rejection(lora_gateways, lora_injector):
    assert lora_injector, (
        "Set FIELDRADIO_LORA_INJECT_COMMAND for raw-frame injection. "
        "It must accept {gateway}, {seq}, {hex} and transmit the exact same "
        "authenticated packet twice. Without an injection path, replay behavior "
        "cannot be honestly tested."
    )
    a, b = lora_gateways
    frame = os.getenv("FIELDRADIO_LORA_REPLAY_FRAME_HEX", "").strip()
    seq = os.getenv("FIELDRADIO_LORA_REPLAY_SEQ", "").strip()
    assert frame and seq.isdigit(), (
        "Set FIELDRADIO_LORA_REPLAY_FRAME_HEX and FIELDRADIO_LORA_REPLAY_SEQ."
    )
    command = lora_injector.format(gateway="A", seq=seq, hex=frame)
    first = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=30)
    second = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=30)
    assert first.returncode == 0 and second.returncode == 0, (
        f"Raw injection failed: first={first.stderr[-300:]}, second={second.stderr[-300:]}"
    )
    # Replay rejection is verified by the receiver's packet/replay counters.
    time.sleep(2)
    capture = b.get(f"{b.base_url}/api/capture/dump", timeout=10)
    assert capture.status_code == 200
    payload = capture.text.lower()
    assert "replay" in payload or "duplicate" in payload, (
        "Receiver capture/log contains no replay/duplicate evidence after two "
        "identical authenticated frames."
    )


@pytest.mark.hil
def test_fragment_reassembly(lora_gateways):
    a, b = lora_gateways
    payload = "FRAG-" + uuid.uuid4().hex + ("X" * 700)
    before = len(_messages(b))
    result = _send_text(a, payload)
    assert result.get("sent") is True, f"Large message was not queued: {result}"
    deadline = time.monotonic() + float(os.getenv("FIELDRADIO_LORA_HIL_TIMEOUT", "45"))
    while time.monotonic() < deadline:
        messages = _messages(b)
        if any(payload in json.dumps(m) for m in messages):
            return
        time.sleep(1)
    pytest.fail(
        "Message > MTU was not reassembled at gateway B. "
        "Check LORA_FRAGMENT_MAX_BYTES, fragment ACK/window, and RF link."
    )


@pytest.mark.hil
def test_sos_ack_flow(lora_gateways):
    a, b = lora_gateways
    r = a.post(f"{a.base_url}{SOS_ENDPOINT}", params={"on": "1"}, timeout=15)
    assert r.status_code == 200, f"SOS activation failed: {r.status_code} {r.text}"
    deadline = time.monotonic() + float(os.getenv("FIELDRADIO_SOS_HIL_TIMEOUT", "30"))
    while time.monotonic() < deadline:
        state = a.get(f"{a.base_url}{SOS_STATUS_ENDPOINT}", timeout=10)
        assert state.status_code == 200
        data = state.json()
        if data.get("acked") is True:
            off = a.post(f"{a.base_url}{SOS_ENDPOINT}", params={"on": "0"}, timeout=10)
            assert off.status_code == 200
            return
        time.sleep(1)
    pytest.fail(
        "SOS was transmitted but no ACK was observed before timeout. "
        "Check the second gateway, matching key/config, radio timing, and ACK path."
    )
