#include <cassert>
#include "BlePeerStore.h"

int main() {
  // Bluetooth Core Vol 3 Part H, Appendix D.7:
  // IRK = ec0234a357c8ad05341010a60a397d9b
  // prand = 00708194, AES output = 159d5fb72ebe2311a48c1bdc c40dfbaa,
  // therefore hash = 0dfbaa and the 48-bit RPA is 70:81:94:0d:fb:aa.
  // NimBLE stores the six address octets reversed internally.
  const uint8_t irk[16] = {
      0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05,
      0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b};
  SensorProtocol::BleAddress rpa{{0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70}, 1};
  assert(BlePeerStore::matchesRpa(rpa, irk));

  SensorProtocol::BleAddress wrong = rpa;
  wrong.bytes[3] ^= 0x01;
  assert(!BlePeerStore::matchesRpa(wrong, irk));

  SensorProtocol::BleAddress staticRandom = rpa;
  staticRandom.bytes[2] = 0xC0;
  assert(!BlePeerStore::matchesRpa(staticRandom, irk));
  return 0;
}
