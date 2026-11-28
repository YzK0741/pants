#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

import scheduler;
import fiber;

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
  pants::scheduler sched(1, true, "test_scheduler");

  std::atomic<int> counter{0};
  constexpr int task_count = 500;

  for (int index = 0; index < task_count; ++index) {
    sched.go([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }

  sched.start();
  sched.stop();

  check(counter.load() == task_count, "all tasks should have run");

  std::atomic<int> batched{0};
  std::vector<pants::fiber::shared> batch;
  for (int index = 0; index < 10; ++index) {
    batch.push_back(std::make_shared<pants::fiber>([&batched] {
      batched.fetch_add(1, std::memory_order_relaxed);
    }));
  }
  sched.add_task(batch);
  sched.start();
  sched.stop();
  check(batched.load() == 10, "range add_task should enqueue every fiber");

  std::atomic<int> switched{0};
  sched.go([&switched, &sched] {
    pants::scheduler_switcher switcher{&sched};
    switched.fetch_add(1, std::memory_order_relaxed);
  });
  sched.start();
  sched.stop();
  check(switched.load() == 1, "scheduler_switcher should not disturb the current scheduler");

    std::atomic<int> yields{0};
  sched.go([&yields] {
    for (int index = 0; index < 5; ++index) {
      yields.fetch_add(1, std::memory_order_relaxed);
      pants::fiber::yield_to_ready();
    }
  });
  sched.start();
  sched.stop();

  check(yields.load() == 5, "yielded fiber should be rescheduled until done");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("scheduler ok");
  return 0;
}
