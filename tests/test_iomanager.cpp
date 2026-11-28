#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

#include <sys/socket.h>
#include <unistd.h>

import iomanager;
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
  int fds[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    std::puts("socketpair failed");
    return 1;
  }

  std::mutex mutex;
  std::condition_variable cv;
  bool io_fired = false;
  bool timer_fired = false;
  char buffer[32]{};

  {
    pants::iomanager io(1, false, "io_test");

    check(io.add_event(fds[0], pants::iomanager::event::read, [&] {
            const ssize_t read_count = ::read(fds[0], buffer, sizeof buffer);
            (void)read_count;
            {
              std::lock_guard lock(mutex);
              io_fired = true;
            }
            cv.notify_all();
          }) == 0,
          "add_event should succeed");

    io.add_timer(pants::timer::duration{20}, [&] {
      {
        std::lock_guard lock(mutex);
        timer_fired = true;
      }
      cv.notify_all();
    });

    const char payload[] = "hello";
    (void)::write(fds[1], payload, sizeof(payload) - 1);

    std::unique_lock lock(mutex);
    const bool both = cv.wait_for(lock, 2s, [&] { return io_fired && timer_fired; });
    check(both, "io event and timer should both fire");
    check(io_fired, "read event callback should have run");
    check(timer_fired, "timer callback should have run");
  }

  {
    pants::iomanager io(1, false, "io_cancel");
    check(io.add_event(fds[0], pants::iomanager::event::read, [] {}) == 0, "add_event for cancel should succeed");
    check(io.del_event(fds[0], pants::iomanager::event::read), "del_event should succeed");
    check(!io.del_event(fds[0], pants::iomanager::event::read), "second del_event should fail");
  }

  (void)::close(fds[0]);
  (void)::close(fds[1]);

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("iomanager ok");
  return 0;
}
