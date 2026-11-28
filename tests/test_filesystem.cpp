#include <cstdio>
#include <string>
#include <vector>

import utility.filesystem;

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
  std::vector<std::string> files;
  pants::list_all_files(files, "/usr/include/c++", ".h");
  check(!files.empty(), "listing headers should find files");

  const std::string directory = "/tmp/pants-filesystem-test";
  check(pants::mkdir(directory), "mkdir should create the directory");
  check(pants::unlink(directory + "/missing", true), "unlink(exist=true) on a missing file should succeed");
  check(!pants::unlink(directory + "/missing", false), "unlink on a missing file should fail");
  check(!pants::is_running_pidfile(directory + "/missing.pid"), "missing pidfile should not be running");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("filesystem ok");
  return 0;
}
