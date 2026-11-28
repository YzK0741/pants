#include <cstdio>
#include <memory>

import fiber;

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
  const auto before = pants::fiber::fiber_count();

  int steps = 0;
  auto worker = std::make_shared<pants::fiber>([&steps] {
    steps = 1;
    pants::fiber::yield_to_ready();
    steps = 2;
  });

  check(worker->get_state() == pants::fiber::state::init, "new fiber should be init");
  check(worker->id() != 0, "fiber id should be non-zero");
  check(worker->stack_size() > 0, "stack size should be non-zero");
  check(pants::fiber::fiber_count() == before + 1, "fiber count should grow");

  const auto first = worker->resume();
  check(first == pants::fiber::state::ready, "first resume should report ready");
  check(steps == 1, "callback should have run until yield");
  check(worker->get_state() == pants::fiber::state::ready, "fiber should be ready after yield");

  const auto second = worker->resume();
  check(second == pants::fiber::state::terminated, "second resume should report terminated");
  check(steps == 2, "callback should have completed");
  check(worker->get_state() == pants::fiber::state::terminated, "fiber should be terminated");

  check(!worker->wake(), "wake on terminated fiber should fail");
  check(worker->get_error() == nullptr, "no error expected on a clean run");

    (void)worker->resume();
  check(worker->get_state() == pants::fiber::state::terminated, "misuse should keep the terminated state");
  const auto error = worker->get_error();
  check(error != nullptr && !error->empty(), "misuse should be saved in get_error()");
  if (error != nullptr) {
    std::printf("saved error: %s\n", error->c_str());
  }

  worker.reset();
  check(pants::fiber::fiber_count() == before, "fiber count should return to baseline");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("fiber ok");
  return 0;
}
