#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct AtomicConfigModel {
  struct Snapshot {
    uint32_t generation;
    uint32_t value;
  };

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return {generation, value};
  }

  bool transaction(uint32_t expectedGeneration, uint32_t nextValue) {
    std::lock_guard<std::mutex> lock(mutex);
    if (generation != expectedGeneration) return false;
    value = nextValue;
    ++generation;
    if (generation == 0) generation = 1;
    return true;
  }

  mutable std::mutex mutex;
  uint32_t generation = 1;
  uint32_t value = 1;
};
}

int main() {
  AtomicConfigModel model;
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};
  std::atomic<unsigned> successfulWrites{0};
  std::vector<std::thread> readers;

  for (unsigned i = 0; i < 8; ++i) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const auto s = model.snapshot();
        // The writer always commits an identical (generation,value) pair.
        if (s.generation != s.value) bad.store(true, std::memory_order_release);
      }
    });
  }

  for (uint32_t value = 2; value <= 1001; ++value) {
    const auto before = model.snapshot();
    if (model.transaction(before.generation, value)) {
      ++successfulWrites;
      const auto after = model.snapshot();
      assert(after.generation == value);
      assert(after.value == value);
    }
  }

  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();

  assert(successfulWrites == 1000U);
  assert(!bad.load(std::memory_order_acquire));
  return 0;
}
