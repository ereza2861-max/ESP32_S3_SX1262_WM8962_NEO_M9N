#include <cassert>
#include <cstdint>
#include <vector>

namespace {
struct Event { uint32_t generation; uint8_t event; };
struct Journal {
  std::vector<Event> records;
  bool append(uint32_t generation, uint8_t event) {
    records.push_back({generation, event});
    return true;
  }
};
void test_append_only() {
  Journal j;
  assert(j.append(1, 0));
  assert(j.append(2, 1));
  assert(j.records.size() == 2);
  assert(j.records[0].generation == 1 && j.records[1].generation == 2);
}
}  // namespace

int main() {
  test_append_only();
  return 0;
}
