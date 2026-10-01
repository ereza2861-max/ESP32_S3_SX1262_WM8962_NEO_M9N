#include "MramStorage.h"
#include "AppState.h"
#include <cstring>

namespace {
constexpr uint32_t SPI_HZ = 40000000UL;
constexpr SPISettings MRAM_SPI_SETTINGS(SPI_HZ, MSBFIRST, SPI_MODE0);
}

bool MramStorage::validRange(uint32_t address, size_t len) const {
  return len > 0 && address < SIZE_BYTES && len <= SIZE_BYTES - address;
}

bool MramStorage::begin() {
  if (!gSpiMutex || Board::MRAM_CS < 0) return false;
  pinMode(Board::MRAM_CS, OUTPUT);
  digitalWrite(Board::MRAM_CS, HIGH);
  ready_ = true;
  return true;
}

bool MramStorage::transfer(uint8_t command, uint32_t address, const uint8_t* tx,
                           uint8_t* rx, size_t len) {
  if (!ready_ || !validRange(address, len)) return false;
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return false;
  SPI.beginTransaction(MRAM_SPI_SETTINGS);
  digitalWrite(Board::MRAM_CS, LOW);
  SPI.transfer(command);
  SPI.transfer(static_cast<uint8_t>((address >> 8) & 0xFF));
  SPI.transfer(static_cast<uint8_t>(address & 0xFF));
  for (size_t i = 0; i < len; ++i) {
    const uint8_t out = tx ? tx[i] : 0;
    const uint8_t in = SPI.transfer(out);
    if (rx) rx[i] = in;
  }
  digitalWrite(Board::MRAM_CS, HIGH);
  SPI.endTransaction();
  return true;
}

uint8_t MramStorage::readStatus() {
  if (!ready_) return 0xFF;
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return 0xFF;
  SPI.beginTransaction(MRAM_SPI_SETTINGS);
  digitalWrite(Board::MRAM_CS, LOW);
  SPI.transfer(CMD_RDSR);
  const uint8_t status = SPI.transfer(0);
  digitalWrite(Board::MRAM_CS, HIGH);
  SPI.endTransaction();
  return status;
}

bool MramStorage::waitReady(uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (readStatus() & SR_WIP) {
    if (millis() - started >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

bool MramStorage::writeEnable() {
  SpiLock lock(pdMS_TO_TICKS(100));
  if (!lock.ok()) return false;
  SPI.beginTransaction(MRAM_SPI_SETTINGS);
  digitalWrite(Board::MRAM_CS, LOW);
  SPI.transfer(CMD_WREN);
  digitalWrite(Board::MRAM_CS, HIGH);
  SPI.endTransaction();
  return (readStatus() & SR_WEL) != 0;
}

bool MramStorage::read(uint32_t address, void* data, size_t len) {
  if (!data || !validRange(address, len)) return false;
  return transfer(CMD_READ, address, nullptr, static_cast<uint8_t*>(data), len);
}

bool MramStorage::write(uint32_t address, const void* data, size_t len) {
  if (!data || !validRange(address, len)) return false;
  if (!writeEnable()) return false;
  if (!transfer(CMD_WRITE, address, static_cast<const uint8_t*>(data), nullptr, len)) return false;
  return waitReady();
}

bool MramStorage::update(uint32_t address, const void* data, size_t len) {
  if (!data || !validRange(address, len)) return false;
  return write(address, data, len);
}
