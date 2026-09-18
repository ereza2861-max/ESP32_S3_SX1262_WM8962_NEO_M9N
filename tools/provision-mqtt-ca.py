from pathlib import Path
import os
import subprocess

Import("env")

ROOT = Path(env.subst("$PROJECT_DIR"))
SECRETS = ROOT / "secrets" / "mqtt_ca.pem"
OUT = ROOT / "include" / "generated" / "MqttCaCert.h"

OUT.parent.mkdir(parents=True, exist_ok=True)

if not SECRETS.is_file():
    OUT.write_text(
        "#pragma once\n"
        "#define MQTT_CA_CUSTOM 0\n"
    )
    print("MQTT CA provisioning: no secrets/mqtt_ca.pem; using built-in EMQX CA fallback.")
else:
    pem = SECRETS.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
    if "-----BEGIN CERTIFICATE-----" not in pem or "-----END CERTIFICATE-----" not in pem:
        raise RuntimeError("secrets/mqtt_ca.pem is not a PEM certificate")
    try:
        subprocess.run(
            ["openssl", "x509", "-in", str(SECRETS), "-noout"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("openssl is required to validate secrets/mqtt_ca.pem") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError("invalid MQTT CA PEM" + (f": {detail}" if detail else "")) from exc

    if not pem.endswith("\n"):
        pem += "\n"
    OUT.write_text(
        "#pragma once\n"
        "#define MQTT_CA_CUSTOM 1\n"
        "static const char MQTT_BROKER_ROOT_CA[] PROGMEM = R\"EOF(\n"
        + pem
        + ")EOF\";\n"
    )
    os.chmod(OUT, 0o600)
    print("MQTT CA provisioning: loaded custom CA from local secrets.")
