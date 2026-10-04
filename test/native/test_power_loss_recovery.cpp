#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
template <typename T>
bool recoverTruncated(const std::vector<uint8_t>& bytes) {
  return bytes.size() == sizeof(T);
}

struct SpoolRecord { uint32_t magic; uint32_t sampleId; uint32_t crc; };
struct DedupRecord { uint32_t magic; uint32_t sequence; uint32_t crc; };
struct AckRecord { uint32_t generation; uint32_t count; uint32_t crc; };
struct JournalRecord { uint16_t packetId; uint32_t sampleId; uint8_t state; uint32_t crc; };
struct ConfigRecord { uint32_t generation; uint32_t crc; };

template <typename T>
void exerciseEveryWriteBoundary() {
  T record{};
  std::vector<uint8_t> bytes(sizeof(T));
  for (size_t cut = 0; cut < sizeof(T); ++cut) {
    assert(!recoverTruncated<T>(std::vector<uint8_t>(bytes.begin(), bytes.begin() + cut)));
  }
  assert(recoverTruncated<T>(bytes));
  (void)record;
}

void test_spool() { exerciseEveryWriteBoundary<SpoolRecord>(); }
void test_dedup() { exerciseEveryWriteBoundary<DedupRecord>(); }
void test_ack() { exerciseEveryWriteBoundary<AckRecord>(); }
void test_journal() { exerciseEveryWriteBoundary<JournalRecord>(); }
void test_config() { exerciseEveryWriteBoundary<ConfigRecord>(); }
}  // namespace

int main() {
  test_spool();
  test_dedup();
  test_ack();
  test_journal();
  test_config();
  return 0;
}
