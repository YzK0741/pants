#include <cstdio>
#include <source_location>
#include <string>
#include <string_view>

import utility;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

[[nodiscard]] std::string read_file(const char* path) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return {};
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::string content(static_cast<std::size_t>(size), '\0');
  const std::size_t read = std::fread(content.data(), 1, content.size(), file);
  std::fclose(file);
  if (read != content.size()) {
    return {};
  }
  return content;
}

} // namespace

int main() {
  constexpr const char* path = "log_basic.log";
  std::remove(path);

  check(utility::set_local_target(path).has_value(), "set_local_target failed");
  utility::set_global_level(utility::log_level::debug);

  (void)utility::log_info(std::source_location::current(), "info {}", 1);
  (void)utility::log_warning(std::source_location::current(), "warn {}", 2);

  utility::set_global_level(utility::log_level::warning);
  (void)utility::log_info(std::source_location::current(), "filtered {}", 3);
  (void)utility::log_error(std::source_location::current(), "err {}", 4);

  check(utility::is_enabled(utility::log_level::error), "error level should be enabled");
  check(!utility::is_enabled(utility::log_level::info), "info level should be filtered");

  check(utility::flush().has_value(), "flush failed");
  check(utility::flush_local().has_value(), "flush_local failed");

  const std::string content = read_file(path);
  check(!content.empty(), "log file is empty");
  check(content.find("[INFO]") != std::string::npos, "missing INFO line");
  check(content.find("info 1") != std::string::npos, "missing info payload");
  check(content.find("[WARNING]") != std::string::npos, "missing WARNING line");
  check(content.find("warn 2") != std::string::npos, "missing warn payload");
  check(content.find("[ERROR]") != std::string::npos, "missing ERROR line");
  check(content.find("err 4") != std::string::npos, "missing err payload");
  check(content.find("filtered 3") == std::string::npos, "filtered line leaked into file");
  check(utility::dropped_count() == 0, "unexpected dropped messages");
  check(utility::to_string(utility::log_level::error) == "ERROR", "level to_string mismatch");
  check(utility::to_string(utility::log_errc::queue_full) == "queue_full", "errc to_string mismatch");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("log_basic ok");
  return 0;
}
