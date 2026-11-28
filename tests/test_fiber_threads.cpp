#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

import fiber;
import fiber.stack;
import scheduler;

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
  int fiber_count = 2000;
  int yield_count = 8;
  int thread_count = 4;
  if (const char* env = std::getenv("PANTS_FIBERS"); env != nullptr) {
    fiber_count = std::atoi(env);
  }
  if (const char* env = std::getenv("PANTS_YIELDS"); env != nullptr) {
    yield_count = std::atoi(env);
  }
  if (const char* env = std::getenv("PANTS_THREADS"); env != nullptr) {
    thread_count = std::atoi(env);
  }
  std::fprintf(stderr, "[test] threads=%d yields=%d\n", thread_count, yield_count);

  if (const char* mode = std::getenv("PANTS_LAZY"); mode != nullptr) {
    pants::fiber_stack_manager::instance().set_lazy_slots(std::string_view(mode) != "0");
  }

  std::atomic<int> completed{0};
  pants::scheduler sched(thread_count, false, "fiber-stress");
  sched.start();
  std::fprintf(stderr, "[test] scheduler started\n");

  pants::fiber::option options{};
  if (const char* mode = std::getenv("PANTS_MODE"); mode != nullptr && std::string_view(mode) == "dedicated") {
    options.stack_mode = pants::stack_mode::dedicated;
  }

  std::vector<pants::fiber::shared> fibers;
  fibers.reserve(fiber_count);
  for (int index = 0; index < fiber_count; ++index) {
    fibers.push_back(std::make_shared<pants::fiber>([&completed, yield_count] {
      for (int round = 0; round < yield_count; ++round) {
        (void)pants::fiber::yield_to_ready();
      }
      completed.fetch_add(1, std::memory_order_release);
    }, options));
  }
  sched.add_task(fibers);
  std::fprintf(stderr, "[test] tasks added\n");

  for (int attempt = 0; attempt < 2000 && completed.load(std::memory_order_acquire) < fiber_count; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  check(completed.load(std::memory_order_acquire) == fiber_count, "all fibers should complete across threads");

  std::fprintf(stderr, "[test] completed, stopping\n");
  sched.stop();
  std::fprintf(stderr, "[test] stopped\n");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("fiber threads ok");
  return 0;
}
