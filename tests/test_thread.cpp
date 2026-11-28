#include <cstdio>
#include <string>

import utility.thread;

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
  check(pants::thread_id() != 0, "thread id should be available");

  pants::set_thread_name("thread-test");
  check(pants::thread_name() == "thread-test", "thread name should be recorded");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("thread ok");
  return 0;
}
