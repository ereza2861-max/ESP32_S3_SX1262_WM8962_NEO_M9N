#include <cassert>
#include <atomic>
#include <thread>
#include <vector>

struct ConfigStore {
  struct State {
    unsigned generation = 0;
    unsigned value = 0;
  };
  std::atomic<unsigned> generation{0};
  std::atomic<unsigned> value{0};

  bool commit(unsigned expected, unsigned nextValue) {
    unsigned current = generation.load(std::memory_order_acquire);
    if (current != expected) return false;
    if (!generation.compare_exchange_strong(current, expected + 1,
                                             std::memory_order_acq_rel)) {
      return false;
    }
    value.store(nextValue, std::memory_order_release);
    return true;
  }
};

int main() {
  ConfigStore store;
  constexpr unsigned threads = 3;  // WebUI, BLE, MQTT
  std::vector<std::thread> workers;
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  std::atomic<unsigned> successes{0};
  for (unsigned i = 0; i < threads; ++i) {
    workers.emplace_back([&, i] {
      const unsigned snapshot = store.generation.load(std::memory_order_acquire);
      ++ready;
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      if (store.commit(snapshot, i + 1)) ++successes;
    });
  }
  while (ready.load(std::memory_order_acquire) != threads) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& t : workers) t.join();
  assert(successes == 1);
  assert(store.generation == 1);
  assert(store.value >= 1 && store.value <= threads);
  return 0;
}
