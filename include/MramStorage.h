#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "BoardConfig.h"

class MramStorage {
public:
  static constexpr uint32_t SIZE_BYTES = 32768UL;
  static constexpr uint32_t MAX_ADDRESS = SIZE_BYTES - 1U;

  bool begin();
  bool read(uint32_t address, void* data, size_t len);
  bool write(uint32_t address, const void* data, size_t len);
  bool update(uint32_t address, const void* data, size_t len);
  bool ready() const { return ready_; }

private:
  static constexpr uint8_t CMD_WREN = 0x06;
  static constexpr uint8_t CMD_WRDI = 0x04;
  static constexpr uint8_t CMD_RDSR = 0x05;
  static constexpr uint8_t CMD_READ = 0x03;
  static constexpr uint8_t CMD_WRITE = 0x02;
  static constexpr uint8_t SR_WIP = 0x01;
  static constexpr uint8_t SR_WEL = 0x02;

  bool ready_ = false;
  bool validRange(uint32_t address, size_t len) const;
  bool transfer(uint8_t command, uint32_t address, const uint8_t* tx,
                uint8_t* rx, size_t len);
  bool writeEnable();
  uint8_t readStatus();
  bool waitReady(uint32_t timeoutMs = 10);
};
