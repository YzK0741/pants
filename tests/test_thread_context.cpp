#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>
#include <thread>

import base;

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
  auto& info = utility::thread_info::get_local();

  check(info.id() != 0, "main thread id is zero");
  check(info.name().empty(), "name should start empty");
  check(info.target() == stdout, "target should default to stdout");

  info.set_name("main");
  check(info.name() == "main", "name was not stored");
  check(info.name_holder() != nullptr, "name holder should be set");

  std::uint64_t worker_id = 0;
  bool worker_named = false;
  bool worker_isolated = false;

  std::thread worker([&] {
    auto& local = utility::thread_info::get_local();
    worker_id = local.id();
    worker_isolated = local.name().empty();
    local.set_name("worker");
    worker_named = local.name() == "worker";
  });
  worker.join();

  check(worker_id != 0, "worker thread id is zero");
  check(worker_id != info.id(), "thread ids should differ");
  check(worker_isolated, "worker thread should not see main thread name");
  check(worker_named, "worker thread name was not stored");
  check(info.name() == "main", "main thread name was clobbered");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("thread_context ok");
  return 0;
}
