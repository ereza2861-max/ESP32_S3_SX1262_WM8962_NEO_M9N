#pragma once

#include "SensorRegistry.h"
#include <Arduino.h>

namespace SensorsExample {

struct PinConfig {
  int batteryAdcPin;
  int digitalPin;
};

bool begin(const PinConfig& pins);
void registerSensors(SensorRegistry& registry, size_t sensorCount);
void sample(SensorRegistry& registry);
void setBatteryAdcPin(int pin);
void setDigitalPin(int pin);
int batteryAdcPin();
int digitalPin();

}  // namespace SensorsExample
