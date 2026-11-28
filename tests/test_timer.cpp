#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

import timer;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

class probe_manager final : public pants::timer_manager {
public:
  void on_timer_inserted_at_front() override {
    ++front_insertions;
  }

  int front_insertions{0};
};

using namespace std::chrono_literals;

} // namespace

int main() {
  probe_manager manager;

  check(!manager.has_timer(), "new manager should have no timer");
  check(!manager.get_next_timeout().has_value(), "no timeout without timers");

  int fired = 0;
  const auto one_shot = manager.add_timer(pants::timer::duration{10}, [&fired] { ++fired; });
  check(one_shot != nullptr, "one-shot timer should be created");
  check(manager.has_timer(), "manager should report a timer");
  check(manager.front_insertions == 1, "first timer should trigger front insertion");
  check(manager.get_next_timeout().has_value(), "timeout should be available");

  std::this_thread::sleep_for(20ms);
  std::vector<pants::timer::shared> expired;
  manager.list_expired_cb(expired);
  check(expired.size() == 1, "one timer should be expired");
  for (auto& entry : expired) {
    entry->fire();
  }
  check(fired == 1, "one-shot callback should have fired");
  check(!manager.has_timer(), "one-shot timer should be removed");

  int recurring_fired = 0;
  const auto recurring = manager.add_timer(pants::timer::duration{5}, [&recurring_fired] { ++recurring_fired; }, true);
  check(recurring != nullptr, "recurring timer should be created");

  std::this_thread::sleep_for(10ms);
  std::vector<pants::timer::shared> recurring_expired;
  manager.list_expired_cb(recurring_expired);
  check(!recurring_expired.empty(), "recurring timer should expire");
  for (auto& entry : recurring_expired) {
    entry->fire();
  }
  check(manager.has_timer(), "recurring timer should stay registered");
  check(recurring->cancel(), "cancel on recurring timer should succeed");
  check(!manager.has_timer(), "cancelled timer should be gone");
  check(!recurring->cancel(), "second cancel should fail");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("timer ok");
  return 0;
}
