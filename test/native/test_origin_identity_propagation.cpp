#include <cassert>
#include <cstdint>
#include <cstring>

namespace {
struct RemoteTelemetry {
  uint32_t nodeId = 0;
  uint32_t originNodeId = 0;
  uint32_t sourceSequence = 0;
};

struct SpoolRecord {
  uint32_t nodeId = 0;
  uint32_t originNodeId = 0;
  uint32_t sourceSequence = 0;
};

static SpoolRecord bridge(const RemoteTelemetry& remote) {
  SpoolRecord out{};
  out.nodeId = remote.nodeId;
  out.originNodeId = remote.originNodeId;
  out.sourceSequence = remote.sourceSequence;
  return out;
}

void test_origin_is_not_replaced_by_transport_source() {
  RemoteTelemetry remote{100U, 200U, 300U};
  const SpoolRecord persisted = bridge(remote);
  assert(persisted.nodeId == 100U);
  assert(persisted.originNodeId == 200U);
  assert(persisted.sourceSequence == 300U);
}

void test_direct_telemetry_keeps_zero_origin() {
  RemoteTelemetry remote{100U, 0U, 301U};
  const SpoolRecord persisted = bridge(remote);
  assert(persisted.originNodeId == 0U);
}
}  // namespace

int main() {
  test_origin_is_not_replaced_by_transport_source();
  test_direct_telemetry_keeps_zero_origin();
  return 0;
}
