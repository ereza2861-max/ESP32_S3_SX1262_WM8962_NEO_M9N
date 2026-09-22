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

struct ConfigTransactionHarness {
  int persisted = 0;
  int runtime = 0;
  int previousPersisted = 0;
  int previousRuntime = 0;
  bool journalPending = false;

  bool commit(int candidate, bool subsystemApplyOk) {
    previousPersisted = persisted;
    previousRuntime = runtime;
    journalPending = true;
    persisted = candidate;
    if (subsystemApplyOk) {
      runtime = candidate;
      journalPending = false;
      return true;
    }
    runtime = previousRuntime;
    persisted = previousPersisted;
    journalPending = false;
    return false;
  }

  void recoverAfterPowerLoss() {
    if (journalPending) {
      persisted = previousPersisted;
      runtime = previousRuntime;
      journalPending = false;
    }
  }
};
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

  assert(deliverLora(false, radioBusy));
  assert(radioBusy.mqttDelivered && radioBusy.loraDelivered);

  ConfigTransactionHarness transaction;
  transaction.persisted = 10;
  transaction.runtime = 10;

  assert(transaction.commit(20, true));
  assert(transaction.persisted == 20);
  assert(transaction.runtime == 20);
  assert(!transaction.journalPending);

  assert(!transaction.commit(30, false));
  assert(transaction.persisted == 20);
  assert(transaction.runtime == 20);
  assert(!transaction.journalPending);

  // Failure injection for the durable boundary: a power loss after the
  // candidate persistence but before apply must recover the previous state.
  transaction.previousPersisted = transaction.persisted;
  transaction.previousRuntime = transaction.runtime;
  transaction.journalPending = true;
  transaction.persisted = 40;
  transaction.recoverAfterPowerLoss();
  assert(transaction.persisted == 20);
  assert(transaction.runtime == 20);
  assert(!transaction.journalPending);

  return 0;
}
