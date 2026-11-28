# include <atomic>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

import mutex;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

} // namespace

int main() {
  pants::cpu_relax();

  pants::spinlock lock;
  check(lock.try_lock(), "try_lock on a free spinlock should succeed");
  check(!lock.try_lock(), "second try_lock should fail while held");
  lock.unlock();

  constexpr int thread_count = 4;
  constexpr int iterations = 20000;
  long counter = 0;

  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int index = 0; index < thread_count; ++index) {
    workers.emplace_back([&] {
      for (int round = 0; round < iterations; ++round) {
        pants::spinlock_guard guard{lock};
        ++counter;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  check(counter == static_cast<long>(thread_count) * iterations, "counter should match total iterations");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("mutex ok");
  return 0;
}