#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>

import base;

int main() {
  const pid_t child = ::fork();
  if (child < 0) {
    std::puts("fork failed");
    return 1;
  }
  if (child == 0) {
    utility::panic("panic test: expected abort");
    std::_Exit(0);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    std::puts("waitpid failed");
    return 1;
  }

  if (!WIFSIGNALED(status)) {
    std::printf("child exited normally (status %d), expected SIGABRT\n", WEXITSTATUS(status));
    return 1;
  }
  if (WTERMSIG(status) != SIGABRT) {
    std::printf("child terminated by signal %d, expected SIGABRT\n", WTERMSIG(status));
    return 1;
  }

  std::puts("panic_aborts ok");
  return 0;
}
