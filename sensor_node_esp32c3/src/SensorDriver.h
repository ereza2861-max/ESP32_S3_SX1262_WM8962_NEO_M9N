#pragma once

#include "SensorProtocol.h"
#include <cstdint>

struct DriverConfig {
  uint8_t driverType = 0;
  uint16_t sensorId = 0;
  uint8_t pinSda = 0;
  uint8_t pinScl = 0;
  uint8_t i2cAddr = 0;
  uint16_t registerAddr = 0;
  uint8_t dataWidth = 0;
  uint32_t periodMs = 1000;
  uint16_t flags = 0;
};

class SensorDriver {
public:
  virtual bool begin(const DriverConfig& config) = 0;
  virtual SensorProtocol::SensorType type() const = 0;
  virtual uint16_t sensorId() const = 0;
  virtual bool read(float& value, uint8_t& quality) = 0;
  virtual SensorProtocol::SensorDescriptor descriptor() const = 0;
  virtual ~SensorDriver() = default;
};
