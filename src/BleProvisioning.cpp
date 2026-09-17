#include "BleProvisioning.h"

bool BleProvisioning::begin(const String&) {
  // Provisioning is intentionally not advertised as available until the
  // authenticated NimBLE service is actually implemented. Returning true from
  // this scaffold would make callers treat an unprovisioned service as ready.
  provisioned_ = false;
  return false;
}

void BleProvisioning::task() {
  // TODO: service authenticated provisioning requests.
}

bool BleProvisioning::isProvisioned() const {
  return provisioned_;
}
