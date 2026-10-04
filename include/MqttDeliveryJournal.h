#pragma once
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include "Config.h"

class MqttDeliveryJournal {
public:
  enum class State : uint8_t { PENDING = 1, DELIVERED = 2, PENDING_RETRY = 3 };
  struct Entry { uint16_t packetId=0; uint32_t sampleId=0; uint8_t state=0; uint8_t reserved[3]{}; };
  bool begin();
  bool pending(uint16_t packetId, uint32_t sampleId);
  bool complete(uint16_t packetId, bool delivered);
  template <typename Callback>
  bool recover(Callback&& cb) {
    // PENDING and PENDING_RETRY both mean that broker delivery is uncertain.
    // Remove the stale packet-id journal entry after recovery; the SD spool
    // remains authoritative and will retry the same deterministic sampleId.
    bool changed = false;
    for (auto& e : entries_) {
      if (!e.valid || !e.entry.sampleId ||
          (e.entry.state != static_cast<uint8_t>(State::PENDING) &&
           e.entry.state != static_cast<uint8_t>(State::PENDING_RETRY))) continue;
      if (!cb(e.entry.packetId, e.entry.sampleId)) return false;
      e = Cached{};
      changed = true;
    }
    return !changed || persistAll();
  }

  String statusJson() const;
private:
  struct DiskEntry {
    Entry entry{};
    uint32_t generation=0;
    uint32_t crc=0;
    uint8_t reserved[4]{};
  } __attribute__((packed));
  static constexpr uint16_t BANK0 = 0x3440;
  static constexpr uint16_t BANK1 = 0x3840;
  static constexpr uint16_t BANK_BYTES = 1024;
  static constexpr size_t MAX_ENTRIES = 32;
  struct Cached { Entry entry{}; uint32_t generation=0; bool valid=false; };
  Cached entries_[MAX_ENTRIES]{};
  uint32_t generation_=0;
  uint8_t activeBank_=0;
  bool ready_=false;
  static uint32_t crc32(const void* data, size_t len);
  bool persistAll();
};
