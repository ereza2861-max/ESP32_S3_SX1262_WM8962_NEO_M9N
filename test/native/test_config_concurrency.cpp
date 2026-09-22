#include <cassert>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class ConfigManagerHarness {
 public:
  struct Snapshot { unsigned generation = 0; unsigned value = 0; };
  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {generation_, value_};
  }
  bool commit(unsigned expected, unsigned nextValue) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ != expected) return false;
    value_ = nextValue;
    ++generation_;
    if (generation_ == 0) generation_ = 1;
    return true;
  }
  bool transaction(unsigned expected, unsigned nextValue, bool applyOk) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ != expected) return false;
    const unsigned previous = value_;
    value_ = nextValue;
    ++generation_;
    if (generation_ == 0) generation_ = 1;
    if (applyOk) return true;
    value_ = previous;
    ++generation_;
    if (generation_ == 0) generation_ = 1;
    return false;
  }
 private:
  mutable std::mutex mutex_;
  unsigned generation_ = 0;
  unsigned value_ = 0;
};

int main() {
  ConfigManagerHarness manager;
  constexpr unsigned writers = 4;
  constexpr unsigned readers = 4;
  std::vector<std::thread> workers;
  std::atomic<unsigned> writerSnapshots{0};
  std::atomic<unsigned> readersReady{0};
  std::atomic<bool> startWrites{false};
  std::atomic<unsigned> successes{0};
  std::atomic<bool> badSnapshot{false};
  std::vector<ConfigManagerHarness::Snapshot> snapshots(writers);

  for (unsigned i = 0; i < writers; ++i) {
    workers.emplace_back([&, i] {
      snapshots[i] = manager.snapshot();
      ++writerSnapshots;
      while (!startWrites.load(std::memory_order_acquire)) std::this_thread::yield();
      if (manager.commit(snapshots[i].generation, i + 1)) ++successes;
    });
  }

  for (unsigned i = 0; i < readers; ++i) {
    workers.emplace_back([&] {
      ++readersReady;
      while (!startWrites.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned n = 0; n < 1000; ++n) {
        const auto snapshot = manager.snapshot();
        if (snapshot.generation != 0 && snapshot.value > writers) badSnapshot = true;
      }
    });
  }

  while (writerSnapshots.load(std::memory_order_acquire) != writers ||
         readersReady.load(std::memory_order_acquire) != readers)
    std::this_thread::yield();
  startWrites.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();

  assert(successes == 1);
  assert(!badSnapshot);
  const auto committed = manager.snapshot();
  assert(committed.generation == 1);
  assert(committed.value >= 1 && committed.value <= writers);

  const unsigned beforeFailure = committed.value;
  const unsigned beforeGeneration = committed.generation;
  assert(!manager.transaction(beforeGeneration, 99, false));
  const auto rolledBack = manager.snapshot();
  assert(rolledBack.value == beforeFailure);
  assert(rolledBack.generation == beforeGeneration + 2);
  return 0;
}
