#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {
#pragma pack(push, 1)
struct DriverEntry {
  uint8_t driverType;
  uint16_t sensorId;
  uint8_t pinSda;
  uint8_t pinScl;
  uint8_t i2cAddr;
  uint8_t reserved[3];
  uint32_t periodMs;
  uint16_t flags;
};

struct DriverConfig {
  uint32_t magic;
  uint8_t version;
  uint8_t count;
  uint16_t reserved;
  DriverEntry entries[8];
  uint32_t crc32;
};
#pragma pack(pop)

static uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1U) ^ (0xEDB88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1U))));
  }
  return crc ^ 0xFFFFFFFFU;
}
}

int main() {
  static_assert(sizeof(DriverEntry) == 15, "DriverEntry layout changed");
  static_assert(sizeof(DriverConfig) == 4 + 1 + 1 + 2 + 8 * 15 + 4,
                "versioned sensor configuration layout changed");

  DriverConfig cfg{};
  cfg.magic = 0x47464353UL;
  cfg.version = 1;
  cfg.count = 1;
  cfg.entries[0].driverType = 4;
  cfg.entries[0].sensorId = 42;
  cfg.entries[0].pinSda = 8;
  cfg.entries[0].pinScl = 9;
  cfg.entries[0].i2cAddr = 0x76;
  cfg.entries[0].reserved[0] = 0x10;
  cfg.entries[0].reserved[2] = 2;
  cfg.entries[0].periodMs = 1000;
  cfg.crc32 = crc32(reinterpret_cast<const uint8_t*>(&cfg), offsetof(DriverConfig, crc32));

  assert(cfg.crc32 == crc32(reinterpret_cast<const uint8_t*>(&cfg), offsetof(DriverConfig, crc32)));
  assert(cfg.entries[0].sensorId == 42);
  assert(cfg.entries[0].reserved[2] == 2);
  return 0;
}
