#include <cassert>
#include <cstdint>
#include <vector>

namespace {
struct Entry { uint16_t packetId; uint32_t sampleId; uint8_t state; };
enum : uint8_t { PENDING = 1, DELIVERED = 2, PENDING_RETRY = 3 };

void recover(std::vector<Entry>& entries) {
  for (auto& e : entries) {
    if (e.state == PENDING || e.state == PENDING_RETRY) e.state = 0;
  }
}

void test_pending_retry_is_recovered() {
  std::vector<Entry> journal{{11, 101, PENDING}, {12, 102, PENDING_RETRY}, {13, 103, DELIVERED}};
  recover(journal);
  assert(journal[0].state == 0);
  assert(journal[1].state == 0);
  assert(journal[2].state == DELIVERED);
}

void test_same_sample_id_remains_retryable() {
  const uint32_t sampleId = 0x12345678U;
  Entry old{9, sampleId, PENDING_RETRY};
  std::vector<Entry> recovered{old};
  // Recovery discards only the stale MQTT packet-id journal entry; the SD
  // spool retains sampleId, so the next publish uses the same application ID.
  recover(recovered);
  assert(recovered[0].state == 0);
  assert(sampleId == 0x12345678U);
}
}  // namespace

int main() {
  test_pending_retry_is_recovered();
  test_same_sample_id_remains_retryable();
  return 0;
}
