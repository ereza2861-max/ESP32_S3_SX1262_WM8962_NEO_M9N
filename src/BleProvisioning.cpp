#include "BleProvisioning.h"

bool BleProvisioning::begin(const String&) {
  // TODO: implement NimBLE-Arduino provisioning service for WiFi + LoRaWAN.
  // Never expose existing credentials in advertisements or unauthenticated reads.
  provisioned_ = false;
  return true;
}

void BleProvisioning::task() {
  // TODO: service authenticated provisioning requests.
}

bool BleProvisioning::isProvisioned() const {
  return provisioned_;
}
