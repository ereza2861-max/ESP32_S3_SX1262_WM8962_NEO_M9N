#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * Routes a successful MQTT PUBACK back to the durable sensor spool boundary.
 *
 * The MQTT transport task calls onPublishSuccess() only after a PUBACK packet
 * whose packet identifier exactly matches the QoS1 publish. A small protected
 * packetId -> sampleId table bridges that transport event to the durable spool.
 */
class MqttAckRouter {
public:
  using Callback = void(*)(uint16_t packetId, void* ctx);

  static void setCallback(Callback cb, void* ctx);
  static bool registerPending(uint16_t packetId, uint32_t sampleId);
  static void forgetPending(uint16_t packetId);
  static bool takeSampleId(uint16_t packetId, uint32_t& sampleId);
  static void onPublishSuccess(uint16_t packetId);

private:
  static constexpr size_t MAX_PENDING = 16;
  struct Pending {
    uint16_t packetId = 0;
    uint32_t sampleId = 0;
    bool valid = false;
  };

  static bool ensureMutex();
  static SemaphoreHandle_t mutex_;
  static Pending pending_[MAX_PENDING];
  static Callback callback_;
  static void* callbackCtx_;
};
