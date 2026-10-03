#include <cassert>
#include <cstdint>

namespace {
struct RemoteRecord {
  uint32_t sourceSequence = 0;
  bool durable = false;
  bool acked = false;
};

static bool admitAndAck(RemoteRecord& record, bool appendSucceeds) {
  if (!appendSucceeds) return false;
  record.durable = true;
  record.acked = true;
  return true;
}

void test_ack_requires_durable_append() {
  RemoteRecord record{42U, false, false};
  assert(!admitAndAck(record, false));
  assert(!record.durable);
  assert(!record.acked);
}

void test_ack_follows_durable_append() {
  RemoteRecord record{42U, false, false};
  assert(admitAndAck(record, true));
  assert(record.durable);
  assert(record.acked);
}

void test_retryable_failure_is_not_drop() {
  RemoteRecord record{42U, false, false};
  const bool first = admitAndAck(record, false);
  assert(!first);
  // The production bridge returns the same object to the remote RAM queue.
  assert(record.sourceSequence == 42U);
}
}  // namespace

int main() {
  test_ack_requires_durable_append();
  test_ack_follows_durable_append();
  test_retryable_failure_is_not_drop();
  return 0;
}
