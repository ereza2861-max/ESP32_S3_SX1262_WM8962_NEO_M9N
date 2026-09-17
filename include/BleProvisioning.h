#pragma once
#include <Arduino.h>

class BleProvisioning {
public:
  bool begin(const String& deviceName);
  void task();
  bool isProvisioned() const;

private:
  bool provisioned_ = false;
};
