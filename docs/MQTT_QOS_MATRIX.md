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

## PUBACK boundary

For sensor telemetry, `publishSensorData()` only means that the sample entered
the MQTT task queue. It does **not** clear the durable spool.

The exact sequence is:

1. `SensorSpool::peek()` selects the pending durable record.
2. `publishSensorData()` enqueues the record, including `originNodeId`.
3. The MQTT task assigns a QoS1 packet identifier and registers the
   packet-id/sample-id pair with `MqttAckRouter`.
4. `waitForPubAck()` accepts completion only when a broker PUBACK with the exact
   packet identifier is received.
5. Only then does `MqttAckRouter::onPublishSuccess()` invoke the spool callback.
6. The callback clears `DELIVERY_MQTT` for that exact sample.

A timeout, transport failure, packet-id mismatch, or queue failure leaves the spool
record pending for a later retry. There is no optimistic `markDelivered()` at
queue-enqueue time.

## Delivery journal
QoS1 sensor publishes write a PENDING completion marker to MRAM before waiting for PUBACK. After the exact PUBACK, the spool completion callback changes the journal to DELIVERED; if spool completion fails, the journal remains PENDING_RETRY and the spool item remains eligible for retry. MQTT delivery remains at-least-once.
