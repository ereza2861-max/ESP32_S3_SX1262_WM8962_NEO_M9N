#include <cassert>
#include <cstdint>

namespace {
struct DeliveryState {
  bool persisted = false;
  bool mqttDelivered = false;
  bool loraDelivered = false;
  uint32_t drops = 0;
};

bool persist(bool sdWriteOk, DeliveryState& state) {
  if (!sdWriteOk) {
    ++state.drops;
    return false;
  }
  state.persisted = true;
  return true;
}

bool deliverMqtt(bool connected, DeliveryState& state) {
  if (!state.persisted || !connected) return false;
  state.mqttDelivered = true;
  return true;
}

bool deliverLora(bool radioBusy, DeliveryState& state) {
  if (!state.persisted || radioBusy) return false;
  state.loraDelivered = true;
  return true;
}
}  // namespace

int main() {
  DeliveryState sdFail{};
  assert(!persist(false, sdFail));
  assert(!sdFail.persisted);
  assert(sdFail.drops == 1);

  DeliveryState nvsFull{};
  assert(!persist(false, nvsFull));
  assert(!nvsFull.persisted);

  DeliveryState radioBusy{};
  assert(persist(true, radioBusy));
  assert(deliverMqtt(true, radioBusy));
  assert(!deliverLora(true, radioBusy));
  assert(radioBusy.persisted && radioBusy.mqttDelivered);
  assert(!radioBusy.loraDelivered);

  assert(deliverLora(false, radioBusy));
  assert(radioBusy.mqttDelivered && radioBusy.loraDelivered);
  return 0;
}
