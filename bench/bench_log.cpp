#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

import log;

namespace {

void worker(int messages_per_thread, std::atomic<bool>& start) {
  while (!start.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (int index = 0; index < messages_per_thread; ++index) {
    auto result = utility::log_info(std::source_location::current(), "benchmark message {} payload", index);
    while (!result) {
      std::this_thread::yield();
      result = utility::log_info(std::source_location::current(), "benchmark message {} payload", index);
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  const int thread_count = argc > 1 ? std::atoi(argv[1]) : 4;
  const int messages_per_thread = argc > 2 ? std::atoi(argv[2]) : 2000;
  if (thread_count <= 0 || messages_per_thread <= 0) {
    std::fputs("usage: bench_log <threads> <messages-per-thread>\n", stderr);
    return 1;
  }

  if (std::freopen("/tmp/pants_bench.log", "wb", stdout) == nullptr) {
    std::fputs("cannot redirect stdout\n", stderr);
    return 1;
  }

  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(thread_count));
  for (int index = 0; index < thread_count; ++index) {
    workers.emplace_back(worker, messages_per_thread, std::ref(start));
  }

  const auto submit_start = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);

  for (std::thread& worker_thread : workers) {
    worker_thread.join();
  }
  const auto submit_end = std::chrono::steady_clock::now();

  const auto flushed = utility::flush();
  std::fflush(stdout);
  const auto write_end = std::chrono::steady_clock::now();

  const auto submit_ms = std::chrono::duration<double, std::milli>(submit_end - submit_start).count();
  const auto write_ms = std::chrono::duration<double, std::milli>(write_end - submit_start).count();

  std::fprintf(stderr, "pants-log: t=%d n=%d submit=%.2f write=%.2f total=%.2f flush=%d\n",
               thread_count, messages_per_thread, submit_ms, write_ms, write_ms,
               flushed.has_value() ? 1 : 0);
  return 0;
}
