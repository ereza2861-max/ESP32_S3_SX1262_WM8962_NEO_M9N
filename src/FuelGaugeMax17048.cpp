#include "FuelGaugeMax17048.h"
#include <Wire.h>

bool FuelGaugeMax17048::begin() {
  Wire.beginTransmission(0x36);
  available_ = Wire.endTransmission() == 0;
  // TODO: implement MAX17048 register reads and use ADC fallback when absent.
  return available_;
}

void FuelGaugeMax17048::task() {
  // TODO: read VCELL/SOC with bounded I2C operations.
}
