#include <cassert>
#include "Config.h"

namespace {
bool accepts(bool productionBuild, bool secureBootEnabled, bool tlsRequired) {
  return !Config::mqttTlsIsMandatory(productionBuild, secureBootEnabled) || tlsRequired;
}
}

int main() {
  // Six explicit policy cases: production/secure-boot combinations and
  // development plaintext/TLS behavior.
  assert(Config::mqttTlsIsMandatory(false, false) == false);
  assert(Config::mqttTlsIsMandatory(true, false) == true);
  assert(Config::mqttTlsIsMandatory(false, true) == true);
  assert(accepts(false, false, false));
  assert(accepts(false, false, true));
  assert(!accepts(true, false, false));
  assert(accepts(true, false, true));
  assert(!accepts(false, true, false));
  assert(accepts(false, true, true));
  return 0;
}
