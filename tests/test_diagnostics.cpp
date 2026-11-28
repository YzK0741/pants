#include <chrono>
#include <cstdio>
#include <string>

import utility.diagnostics;

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
  check(pants::elapsed_time() >= std::chrono::steady_clock::duration::zero(), "elapsed time should be non-negative");
  check(pants::backtrace(8, 1, "  ").find('\n') != std::string::npos, "backtrace should contain frames");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("diagnostics ok");
  return 0;
}
