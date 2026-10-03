# LoRa Rev-C Level-3: selective ACK/window and routing metrics

> **Project status (2026-09-17):** No node is operating and the PCB has not been fabricated. Level-3 routing documentation describes the protocol/design target; it is not a record of field validation. See `docs/PROJECT_STATUS.md`.

This revision keeps the authenticated LoRa envelope unchanged (packet v2). The
voice reliability changes are carried inside the existing encrypted payload of
`LORA_TYPE_VOICE_ACK`.

## Voice selective ACK

- TX window: 8 voice frames.
- ACK is cumulative plus an 8-bit selective bitmap.
- The receiver ACKs the highest contiguous sequence (`ackBase`) and the next
  eight sequence positions (`ackBitmap`).
- The sender can therefore continue producing 20 ms voice frames without
  waiting for an ACK after every frame.
- Unacknowledged frames are retransmitted independently, up to three attempts.
- Retry timeout is increased for poor RSSI/SNR links.
- The previous 12-byte single-frame ACK remains accepted for mixed-firmware
  migration.

## Payload-compatible destination/next-hop routing

The authenticated packet envelope remains unchanged (`LORA_PROTOCOL_VERSION`
2, or the existing hop-aware v3 envelope). Routing metadata is carried inside
the encrypted payload as an optional 16-byte extension:

- destination node ID
- selected next-hop node ID
- previous-hop node ID
- hop count and extension version/flags

Legacy payloads without the extension remain valid and are still accepted.
New firmware strips the extension before handing the payload to text/voice/SOS
handlers, so application payload formats remain unchanged.

`sendTextTo(destination, text)` provides an explicit destination API. Existing
`sendText(text)` remains broadcast-compatible. ACKs are addressed back to the
originator through the same extension.

## ETX + RSSI/SNR route selection

Each neighbor maintains EWMA RSSI/SNR quality plus TX-attempt/success counters.
A route score combines link quality and ETX; fresh learned routes are preferred,
while a direct destination neighbor is preferred when available. Route entries
and neighbors age out after 120 seconds.

A routed packet teaches the receiver a reverse route to its origin using the
authenticated previous-hop field. This provides lightweight reactive routing
without changing the radio envelope or requiring a separate routing protocol.

## Selective forwarding and loop prevention

Only the node named by `nextHop` forwards a routed unicast packet. Broadcast
packets select one fresh neighbor as the forwarding candidate. Each forward
increments the route hop count and rewrites `previousHop` to the forwarding
node. A packet is dropped when the hop limit is exhausted, when the next-hop
does not match the local node, or when the selected next hop would immediately
return to the previous hop.

The replay cache is authoritative for the authenticated origin sequence.
Forwarding may rewrite the routing extension, but that does **not** make a new
origin frame: the same origin sequence remains replay-protected. The route
extension currently carries only the immediate previous hop and hop count; it
does not encode full path history, so loop prevention still relies on TTL,
origin/previous-hop exclusion, next-hop selection, and replay/dedup state.

## Compatibility and safety

- `LORA_PROTOCOL_VERSION` remains 2 for the base envelope; the existing
  hop-aware v3 envelope remains available.
- Existing encrypted packet header, nonce, source ID, TTL and authentication
  are unchanged.
- The routing extension is encrypted and authenticated with the payload.
- ACK payload versioning is explicit so malformed/old payloads are rejected.
- Window size is bounded at 8 to match the receiver reorder buffer and avoid
  unbounded RAM growth.


## Text fragmentation selective repeat

Text payloads larger than the single-frame budget use a bounded selective-repeat
transaction. The sender keeps at most `LORA_FRAGMENT_WINDOW_SIZE` (8) fragments in flight,
and the receiver returns an 8-bit SACK bitmap beginning at the first missing fragment.
Only missing fragments are retransmitted. A completed message is acknowledged by a SACK
with `baseIndex == fragmentCount`.

Hard limits are enforced at 2048 bytes total and 16 fragments. Reassembly has three
bounded concurrent slots and persists incomplete state to SD. Conflicting duplicate
fragments are rejected rather than replacing authenticated state. Because the project is
greenfield, the fragment ACK payload may use this new 8-byte SACK form without a legacy
fragment-peer compatibility branch.

## Remote sensor telemetry durable-ACK boundary

Authenticated remote sensor telemetry is first placed in the gateway's bounded
RAM handoff queue. The gateway then calls `RemoteTelemetryBridge`, which performs
the durable `SensorSpool::append()` before the LoRa batch ACK is queued.

If the SD/spool append fails, the exact remote telemetry item is returned to the
RAM handoff queue and no ACK is emitted. This makes durable spool admission the
ACK boundary rather than RAM-queue admission.

`originNodeId` is carried from `RemoteSensorTelemetry` into the durable spool
record and remains distinct from the immediate transport/source `nodeId`.
