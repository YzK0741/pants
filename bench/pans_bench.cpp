#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <pans/logger/log.h>

namespace {

void worker(const pans::LoggerPtr& logger, int messages_per_thread, std::atomic<bool>& start) {
  while (!start.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (int index = 0; index < messages_per_thread; ++index) {
    PANS_LOG_FMT_INFO(logger, "benchmark message %d payload", index);
  }
}

} // namespace

int main(int argc, char** argv) {
  const int thread_count = argc > 1 ? std::atoi(argv[1]) : 4;
  const int messages_per_thread = argc > 2 ? std::atoi(argv[2]) : 2000;
  if (thread_count <= 0 || messages_per_thread <= 0) {
    std::puts("usage: pans_bench <threads> <messages-per-thread>");
    return 1;
  }

  auto logger = std::make_shared<pans::Logger>("bench");
  logger->setLevel(pans::LogLevel::Level::LOG_LV_INFO);
  logger->addAppender(pans::MakeFileAppender("/tmp/pans_bench.log"));

  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(thread_count));
  for (int index = 0; index < thread_count; ++index) {
    workers.emplace_back(worker, std::cref(logger), messages_per_thread, std::ref(start));
  }

  const auto submit_start = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);

  for (std::thread& worker_thread : workers) {
    worker_thread.join();
  }
  const auto submit_end = std::chrono::steady_clock::now();

  logger->flush();
  const auto write_end = std::chrono::steady_clock::now();

  const auto submit_ms = std::chrono::duration<double, std::milli>(submit_end - submit_start).count();
  const auto write_ms = std::chrono::duration<double, std::milli>(write_end - submit_start).count();

  std::printf("pans-log: t=%d n=%d submit=%.2f write=%.2f total=%.2f\n",
              thread_count, messages_per_thread, submit_ms, write_ms, write_ms);
  return 0;
}
