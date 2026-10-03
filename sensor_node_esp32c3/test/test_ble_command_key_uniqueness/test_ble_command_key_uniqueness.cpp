#include <cassert>
#include <cstring>
#include "SensorProtocol.h"

static void testBleIdentityTokenUsesFullIdentity() {
  const uint8_t a[6] = {0,1,2,3,4,5};
  const uint8_t b[6] = {0,1,2,3,4,6};
  char ka[15] = {}, kb[15] = {}, kt[15] = {};
  SensorProtocol::makeBleCommandIdentityToken(a, 0, ka);
  SensorProtocol::makeBleCommandIdentityToken(b, 0, kb);
  SensorProtocol::makeBleCommandIdentityToken(a, 1, kt);
  assert(std::strlen(ka) == 13);
  assert(std::strcmp(ka, kb) != 0);
  assert(std::strcmp(ka, kt) != 0);
}

void setup() { testBleIdentityTokenUsesFullIdentity(); }
void loop() {}
