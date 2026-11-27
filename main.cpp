#include <source_location>
import utility;
int main() {
  utility::log_debug(std::source_location::current(), "debug message {}", 1);
  utility::log_info(std::source_location::current(), "info message {}", 2);
  utility::log_warning(std::source_location::current(), "warning message {}", 3);
  utility::log_fatal(std::source_location::current(), "fatal message {}", 4);
  utility::flush();
  return 0;
}
