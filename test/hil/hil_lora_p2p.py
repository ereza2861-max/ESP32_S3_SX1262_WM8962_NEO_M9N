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
    s.base_url = base_url
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


def _wait_ecdh_active(session, timeout=45):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        r = session.get(f"{session.base_url}/api/ecdh/status", timeout=10)
        assert r.status_code == 200, f"ECDH status failed: {r.status_code} {r.text[:300]}"
        last = r.json()
        if (last.get("enabled") is True and last.get("active") is True and
                last.get("peerCount", 0) > 0):
            return last
        time.sleep(1)
    pytest.fail(f"ECDH did not become active before timeout; last={last}")


def _start_capture(session, duration_ms=10000):
    r = session.post(
        f"{session.base_url}/api/capture/start?duration={duration_ms}",
        timeout=10,
    )
    assert r.status_code == 200, f"capture start failed: {r.status_code} {r.text[:300]}"


def _capture(session):
    r = session.get(f"{session.base_url}/api/capture/dump", timeout=10)
    assert r.status_code == 200, f"capture dump failed: {r.status_code} {r.text[:300]}"
    data = r.json()
    assert isinstance(data, list)
    return data


def _wait_for_v5_capture(session, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for entry in _capture(session):
            raw = entry.get("rawHex", "")
            if len(raw) >= 4 and raw[2:4].lower() == "05":
                return raw
        time.sleep(1)
    pytest.fail("No authenticated V5 frame was observed in receiver capture.")


def _wait_for_text(session, marker: str, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if any(marker in json.dumps(m) for m in _messages(session)):
            return
        time.sleep(1)
    pytest.fail(f"Text marker {marker!r} did not arrive before timeout.")


def _seq_from_frame_hex(frame_hex: str) -> int:
    raw = bytes.fromhex(frame_hex)
    assert len(raw) >= 5 and raw[0] == 0xC3, "Unexpected LoRa frame magic."
    return raw[3] | (raw[4] << 8)


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
def test_ecdh_v5_two_node_roundtrip(lora_gateways):
    a, b = lora_gateways
    _wait_ecdh_active(a)
    _wait_ecdh_active(b)
    _start_capture(b, 10000)
    marker = f"G11-{uuid.uuid4().hex}"
    # Send a distinct marker after both nodes advertise ECDH support.
    _send_text(a, marker)
    _wait_for_text(b, marker)
    frame = _wait_for_v5_capture(b)
    assert bytes.fromhex(frame)[1] == 5, "Roundtrip did not use V5/ECDH wire format."


@pytest.mark.hil
def test_ecdh_reboot_rekey_roundtrip(lora_gateways):
    a, b = lora_gateways
    _wait_ecdh_active(a)
    _wait_ecdh_active(b)
    marker = f"G11-REBOOT-{uuid.uuid4().hex}"
    _send_text(a, marker)
    _wait_for_text(b, marker)

    r = b.post(f"{b.base_url}/api/reboot", timeout=5)
    assert r.status_code == 200, f"Gateway B reboot request failed: {r.status_code}"
    time.sleep(3)
    _wait_ecdh_active(b, timeout=60)
    time.sleep(35)

    marker_after = f"G11-AFTER-{uuid.uuid4().hex}"
    _send_text(a, marker_after)
    _wait_for_text(b, marker_after, timeout=60)


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
def test_deep_sleep_wake_and_replay_persistence(lora_gateways, lora_injector):
    assert lora_injector, (
        "Set FIELDRADIO_LORA_INJECT_COMMAND so the test can replay the "
        "pre-sleep authenticated beacon after wake."
    )
    a, b = lora_gateways
    _wait_ecdh_active(a)
    _wait_ecdh_active(b)

    _start_capture(b, 30000)
    # Capture a V3 ECDH beacon before sleep. Its replay state must survive
    # deep sleep, while packets sent during sleep are allowed to be lost.
    beacon = None
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        for entry in _capture(b):
            raw = entry.get("rawHex", "")
            if len(raw) >= 4 and raw[2:4].lower() == "03":
                beacon = raw
                break
        if beacon:
            break
        time.sleep(1)
    assert beacon, "No pre-sleep authenticated V3 beacon captured."

    r = b.post(f"{b.base_url}/api/deep-sleep", timeout=10)
    assert r.status_code == 200, f"Deep-sleep request failed: {r.status_code}"

    # A's next authenticated beacon is the wake stimulus. Do not require data
    # delivery while B is asleep: that packet is intentionally allowed to be lost.
    time.sleep(2)
    _send_text(a, f"G12-SLEEP-LOSS-{uuid.uuid4().hex}")
    _wait_ecdh_active(b, timeout=90)
    # Allow the freshly regenerated ephemeral key to reach A in the next beacon.
    time.sleep(35)

    seq = _seq_from_frame_hex(beacon)
    command = lora_injector.format(gateway="A", seq=seq, hex=beacon)
    first = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=30)
    second = subprocess.run(command, shell=True, text=True, capture_output=True, timeout=30)
    assert first.returncode == 0 and second.returncode == 0, (
        f"Replay injection failed after deep sleep: "
        f"first={first.stderr[-300:]}, second={second.stderr[-300:]}"
    )
    time.sleep(2)
    stats = b.get(f"{b.base_url}/api/dedup/stats", timeout=10)
    assert stats.status_code == 200
    replay_rejects = stats.json().get("replayRejects", 0)
    assert replay_rejects > 0, (
        "No replay rejection was counted after deep-sleep wake; "
        "replay state may have been reset."
    )

    marker = f"G12-AFTER-SLEEP-{uuid.uuid4().hex}"
    _send_text(a, marker)
    _wait_for_text(b, marker, timeout=60)


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
