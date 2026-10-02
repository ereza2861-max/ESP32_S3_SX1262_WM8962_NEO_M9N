#include <cassert>
#include <cstdint>
#include <deque>
// Queue depth remains 16 by frozen decision; a single 12-sensor roster fits without changing it.
static_assert(16 >= 12, "sensor queue depth must cover a 12-sensor roster");

enum class Policy { DROP_NEWEST, DROP_OLDEST };

struct Sample { uint32_t id = 0; };

static bool enqueue(std::deque<Sample>& q, size_t depth, Sample sample,
                    Policy policy, uint32_t& dropped) {
  if (q.size() < depth) { q.push_back(sample); return true; }
  if (policy == Policy::DROP_OLDEST) {
    q.pop_front();
    ++dropped;
    if (q.size() < depth) { q.push_back(sample); return true; }
    return false;
  }
  ++dropped;
  return false;
}

int main() {
  std::deque<Sample> q;
  uint32_t dropped = 0;
  for (uint32_t i = 0; i < 16; ++i) assert(enqueue(q, 16, {i}, Policy::DROP_OLDEST, dropped));
  assert(enqueue(q, 16, {16}, Policy::DROP_OLDEST, dropped));
  assert(dropped == 1 && q.front().id == 1 && q.back().id == 16);

  q.clear(); dropped = 0;
  for (uint32_t i = 0; i < 16; ++i) assert(enqueue(q, 16, {i}, Policy::DROP_NEWEST, dropped));
  assert(!enqueue(q, 16, {16}, Policy::DROP_NEWEST, dropped));
  assert(dropped == 1 && q.front().id == 0 && q.back().id == 15);
  return 0;
}
