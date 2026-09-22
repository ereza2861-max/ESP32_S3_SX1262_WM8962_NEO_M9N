#include <cassert>
#include <atomic>
#include <thread>
#include <vector>

struct ConfigStore {
  std::atomic<unsigned> generation{0};
  bool commit(unsigned expected) {
    unsigned current = generation.load(std::memory_order_acquire);
    if (current != expected) return false;
    return generation.compare_exchange_strong(current, expected + 1,
                                              std::memory_order_acq_rel);
  }
};

int main() {
  ConfigStore store;
  constexpr unsigned threads = 8;
  std::vector<std::thread> workers;
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  std::atomic<unsigned> successes{0};
  for (unsigned i = 0; i < threads; ++i) {
    workers.emplace_back([&] {
      const unsigned snapshot = store.generation.load(std::memory_order_acquire);
      ++ready;
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      if (store.commit(snapshot)) ++successes;
    });
  }
  while (ready.load(std::memory_order_acquire) != threads) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& t : workers) t.join();
  assert(successes == 1);
  assert(store.generation == 1);
  return 0;
}
