# FASE 2 — Apply HIL Tests

FASE 2 adds only new test/support files under `test/hil/`; no existing
firmware source file is changed, so there is no `PATCH.diff` for this phase.

Copy the supplied `test/hil/` directory into the repository:

```bash
cp -a test/hil /path/to/your/repository/test/
```

If copying from the generated package:

```bash
unzip -o FASE2_HIL_FILES.zip -d /path/to/your/repository
```

The resulting files are:

```text
test/hil/conftest.py
test/hil/hil_sensor_ble.py
test/hil/hil_lora_p2p.py
test/hil/requirements.txt
test/hil/README.md
```

Install dependencies:

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r test/hil/requirements.txt
```

Start a local Mosquitto broker and configure the environment variables described
in `test/hil/README.md`.

Run BLE HIL:

```bash
pytest -v -m hil test/hil/hil_sensor_ble.py
```

Run LoRa HIL:

```bash
pytest -v -m hil test/hil/hil_lora_p2p.py
```

Run all HIL:

```bash
pytest -v -m hil test/hil
```

## Deliberate non-skip behavior

The tests use assertions for missing physical prerequisites. A missing serial
port, gateway URL, MQTT broker, pairing helper, node, power-control relay, or
raw LoRa injector produces an explicit failure explaining what is required.

No mock is used for BLE pairing, MQTT notification delivery, sensor discovery,
node loss, or LoRa packet injection.

## Important lab interfaces

The existing firmware exposes gateway serial commands for status/config/LoRaWAN,
but it does not expose raw P2P packet injection. Therefore replay rejection cannot
be truthfully tested from the normal serial console alone. Set
`FIELDRADIO_LORA_INJECT_COMMAND` to a real lab injector.

Likewise, passkey entry is delegated by Bleak to the operating-system Bluetooth
stack. Set `FIELDRADIO_BLE_PAIR_COMMAND` to a real host pairing helper that can
perform the passkey operation. This is intentionally not replaced by a fake
Python callback.

## Source-tree verification

After copying:

```bash
test -f test/hil/conftest.py
test -f test/hil/hil_sensor_ble.py
test -f test/hil/hil_lora_p2p.py
test -f test/hil/requirements.txt
test -f test/hil/README.md
python -m py_compile test/hil/conftest.py test/hil/hil_sensor_ble.py test/hil/hil_lora_p2p.py
```

Expected: all files exist and Python compilation exits with status 0.
