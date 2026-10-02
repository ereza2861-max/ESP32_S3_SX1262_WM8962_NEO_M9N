# MQTT command policy

FieldRadio does not add an MQTT command subscription as part of the audit
closure. MQTT remains an upstream telemetry transport; command/control traffic
is not accepted through MQTT.

Sensor-node control, where enabled by the Q05 decision, uses the additive
authenticated BLE command characteristic pair defined in `SensorProtocol.h`.
This keeps the existing MQTT topic set and broker authorization surface
unchanged.
