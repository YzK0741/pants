//
// Created by 23530 on 2026/11/26.
//
module;

#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

export module utility:log;

import :build;


export namespace utility {

enum class log_level : std::uint8_t {
  debug,
  info,
  warning,
  fatal,
};

enum class log_error : std::uint8_t {
  queue_full,
  fatal_occurred,
  target_open_failed,
  write_failed,
  flush_failed,
};

std::expected<void, log_error> set_target(std::string_view new_target);
void set_name(std::string_view name);
[[nodiscard]] std::string_view get_name();
std::expected<void, log_error> flush();
void set_level(log_level level);
[[nodiscard]] log_level get_level();

}


namespace utility::detail {

[[nodiscard]] bool should_log(log_level level) noexcept;

std::expected<void, log_error> submit_message(
  log_level level,
  std::string message,
  std::source_location location);

template <typename... Args>
std::expected<void, log_error> submit_log(
  log_level level,
  std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  if (!should_log(level)) {
    return {};
  }

  return submit_message(
      level, std::format(message, std::forward<Args>(args)...), location);
}

} // namespace detail

export namespace utility {

template <typename... Args>
std::expected<void, log_error> log_debug(
  [[maybe_unused]] std::source_location location,
  [[maybe_unused]] std::format_string<Args...> message,
  [[maybe_unused]] Args&&... args) {
  if constexpr (debug_enabled) {
    return detail::submit_log(log_level::debug, location, message, std::forward<Args>(args)...);
  }
  return {};
}

template <typename... Args>
std::expected<void, log_error> log_info(
  std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  return detail::submit_log(log_level::info, location, message, std::forward<Args>(args)...);
}

template <typename... Args>
std::expected<void, log_error> log_warning(
  std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  return detail::submit_log(log_level::warning, location, message, std::forward<Args>(args)...);
}

template <typename... Args>
std::expected<void, log_error> log_fatal(
  std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  auto result = detail::submit_message(
      log_level::fatal, std::format(message, std::forward<Args>(args)...), location);
  if (!result) {
    return result;
  }
  return std::unexpected(log_error::fatal_occurred);
}

} // namespace utility
