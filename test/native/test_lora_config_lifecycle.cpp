// ENH-3: Host-only model of the LoRaManager configuration lifecycle.
#include <cassert>
#include <cstdint>

namespace {
struct LoRaConfigModel {
  float freq;
  uint8_t sf;
  uint8_t cr;
  bool adrEnabled;
  uint8_t hopProfile;
  uint8_t replayWindowBits;
  uint32_t generation;
};

LoRaConfigModel state{923.0f, 7, 5, false, 1, 8, 0};

bool applyConfig(const LoRaConfigModel& next, uint32_t expectedGen) {
  if (next.generation != expectedGen + 1U ||
      next.replayWindowBits < 8 ||
      next.sf < 7 || next.sf > 12) {
    return false;
  }
  state = next;
  return true;
}
}

int main() {
  LoRaConfigModel next = state;
  next.generation = 1;
  assert(applyConfig(next, 0));
  assert(state.generation == 1);

  LoRaConfigModel before = state;
  next.generation = 3;
  assert(!applyConfig(next, 1));
  assert(state.generation == before.generation);

  next = state;
  next.generation = 2;
  next.replayWindowBits = 7;
  assert(!applyConfig(next, 1));

  next = state;
  next.generation = 2;
  next.replayWindowBits = 8;
  next.sf = 13;
  assert(!applyConfig(next, 1));

  next = state;
  next.generation = 2;
  next.sf = 7;
  assert(applyConfig(next, 1));
  next.generation = 3;
  assert(applyConfig(next, 2));
  assert(state.generation == 3);

  next = state;
  next.generation = 4;
  next.sf = 13;
  assert(!applyConfig(next, 3));
  next.sf = 7;
  assert(applyConfig(next, 3));
  assert(state.generation == 4);
  return 0;
}
