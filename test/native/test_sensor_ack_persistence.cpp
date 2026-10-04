#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {
struct AckStore {
  bool ready = true;
  uint32_t generation = 0;
  uint32_t count = 2;
  bool persist(const void*, size_t n) {
    if (!ready) return false;
    count = static_cast<uint32_t>(n);
    ++generation;
    return true;
  }
  bool clear() {
    uint8_t empty[8] = {};
    return persist(empty, 0);
  }
};

void test_clear_uses_a_valid_empty_buffer() {
  AckStore store;
  assert(store.clear());
  assert(store.count == 0);
  assert(store.generation == 1);
}
}  // namespace

int main() {
  test_clear_uses_a_valid_empty_buffer();
  return 0;
}
