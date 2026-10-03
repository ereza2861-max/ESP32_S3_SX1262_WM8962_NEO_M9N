#include <cassert>
#include <cstdint>
#include <unordered_map>

namespace {
class DeliveryModel {
 public:
  void enqueue(uint32_t sampleId) { pending_[sampleId] = false; }
  void onQueueAccepted(uint32_t sampleId) { (void)sampleId; /* no completion */ }
  void onPubAck(uint16_t packetId, uint32_t sampleId) {
    packetToSample_[packetId] = sampleId;
    pending_[sampleId] = true;
  }
  bool delivered(uint32_t sampleId) const {
    auto it = pending_.find(sampleId);
    return it != pending_.end() && it->second;
  }
 private:
  std::unordered_map<uint32_t, bool> pending_;
  std::unordered_map<uint16_t, uint32_t> packetToSample_;
};

void test_queue_acceptance_is_not_delivery() {
  DeliveryModel model;
  model.enqueue(0x12345678U);
  model.onQueueAccepted(0x12345678U);
  assert(!model.delivered(0x12345678U));
}

void test_matching_puback_is_delivery_boundary() {
  DeliveryModel model;
  model.enqueue(0x12345678U);
  model.onPubAck(17U, 0x12345678U);
  assert(model.delivered(0x12345678U));
}

void test_unacknowledged_sample_remains_pending() {
  DeliveryModel model;
  model.enqueue(0xCAFEBABEU);
  assert(!model.delivered(0xCAFEBABEU));
}
}  // namespace

int main() {
  test_queue_acceptance_is_not_delivery();
  test_matching_puback_is_delivery_boundary();
  test_unacknowledged_sample_remains_pending();
  return 0;
}
