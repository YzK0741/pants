#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

import fiber;
import hook;
import iomanager;
import scheduler;
import timer;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

using namespace std::chrono_literals;

} // namespace

int main() {
  pants::set_hook_enable(true);
  check(pants::is_hook_enable(), "hook should be enabled");

  int fds[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    std::puts("socketpair failed");
    return 1;
  }

  std::atomic<bool> done{false};
  std::atomic<ssize_t> received{-1};

  {
    pants::iomanager io(1, false, "hook_test");

    auto worker = std::make_shared<pants::fiber>([&] {

      char buffer[16]{};
      const ssize_t count = ::read(fds[0], buffer, sizeof buffer);
      received.store(count, std::memory_order_release);
      done.store(true, std::memory_order_release);
    });

    io.add_task(worker);

    std::this_thread::sleep_for(50ms);

    const char payload[] = "hello";
    (void)::write(fds[1], payload, sizeof(payload) - 1);

    for (int attempt = 0; attempt < 200 && !done.load(std::memory_order_acquire); ++attempt) {
      std::this_thread::sleep_for(10ms);
    }
  }

  check(done.load(std::memory_order_acquire), "hook read should unblock after write");
  check(received.load(std::memory_order_acquire) == 5, "hook read should return payload length");

  (void)::close(fds[0]);
  (void)::close(fds[1]);
  pants::set_hook_enable(false);

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("hook ok");
  return 0;
}
