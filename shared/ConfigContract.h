#pragma once
#include <stddef.h>
#include <stdint.h>

// Platform-neutral constants consumed by the shared ECDH protocol contract.
// Keep these values synchronized with include/Config.h; the gateway header
// contains static_asserts that make divergence a compile-time failure.
#ifndef FIELDRADIO_LORA_ECDH_REKEY_ENABLED
#define FIELDRADIO_LORA_ECDH_REKEY_ENABLED 1
#endif

namespace ConfigContract {
constexpr uint32_t LORA_REKEY_PERIOD_SEC = 3600UL;
constexpr uint8_t LORA_ECDH_KEY_EPOCH_DELTA_CURRENT = 0;
constexpr uint8_t LORA_ECDH_KEY_EPOCH_DELTA_PREVIOUS = 1;
constexpr uint8_t LORA_ECDH_PROTOCOL_VERSION = 1;
constexpr uint8_t LORA_ECDH_BEACON_MAGIC = 0xE2;
constexpr size_t LORA_ECDH_PUBLIC_KEY_BYTES = 32;
constexpr size_t LORA_ECDH_BEACON_BYTES =
    1U + 1U + sizeof(uint32_t) +
    LORA_ECDH_PUBLIC_KEY_BYTES + LORA_ECDH_PUBLIC_KEY_BYTES;
constexpr uint32_t LORA_ECDH_KEY_RETENTION_SEC =
    2UL * LORA_REKEY_PERIOD_SEC;
constexpr uint8_t LORA_TYPE_NEIGHBOR_BEACON = 6;
}
