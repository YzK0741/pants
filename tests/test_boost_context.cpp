#include <cstdio>

#include <boost/context/fiber.hpp>

namespace ctx = boost::context;

int main() {
  bool entered = false;
  bool finished = false;

  ctx::fiber fiber{[&entered, &finished](ctx::fiber&& sink) {
    entered = true;
    finished = true;
    return std::move(sink);
  }};

  fiber = std::move(fiber).resume();

  if (!entered) {
    std::puts("FAIL: fiber body did not run");
    return 1;
  }
  if (!finished) {
    std::puts("FAIL: fiber did not reach the end");
    return 1;
  }

  std::puts("boost_context ok");
  return 0;
}
