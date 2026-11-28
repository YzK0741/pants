#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <thread>

import utility.clock;

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
  const std::string first = std::string(utility::seconds_text());
  check(first.size() == 19, "seconds text should be 19 characters");
  check(first == std::string(utility::seconds_text()), "same second should return the same text");

  const auto past = std::chrono::system_clock::now() - std::chrono::seconds(3600);
  const std::string past_text = std::string(utility::seconds_text(past));
  const std::string expected = std::format("{:%Y-%m-%d %H:%M:%S}",
                                           std::chrono::time_point_cast<std::chrono::seconds>(past));
  check(past_text == expected, "historical timestamp should be formatted on demand");

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  check(std::string(utility::seconds_text()) != first, "text should change after one second");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("clock ok");
  return 0;
}
