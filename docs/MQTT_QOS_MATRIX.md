# MQTT QoS matrix

This document records the additive MQTT delivery contract for the audit decisions.

| Traffic | QoS | Broker acknowledgement | Durable boundary | Notes |
| --- | ---: | --- | --- | --- |
| Sensor telemetry from the local BLE spool | 1 | PUBACK required before the MQTT delivery bit is cleared | SD `SensorSpool` | MQTT remains authoritative for upstream sensor delivery |
| Remote LoRa sensor telemetry | 1 | PUBACK required before the spool record is cleared | SD `SensorSpool` | LoRa reception is authenticated before admission |
| Existing status/health telemetry | existing implementation | existing implementation | unchanged | Q03 does not broaden QoS policy |
| MQTT command subscription | none | n/a | n/a | Q01 explicitly keeps MQTT command subscription disabled |

QoS changes are additive at the application delivery boundary. No MQTT topic
version or wire protocol version is changed by this decision.
