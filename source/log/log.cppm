//
// Created by 23530 on 2026/11/26.
//
module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

export module log;

import base;


export namespace utility {

enum class log_level : std::uint8_t {
  debug,
  info,
  warning,
  error,
  fatal,
};

enum class log_errc : std::uint8_t {
  queue_full,
  fatal_occurred,
  target_open_failed,
  write_failed,
  flush_failed,
};

[[nodiscard]] std::string_view to_string(log_level level);
[[nodiscard]] std::string_view to_string(log_errc error);

std::expected<void, log_errc> set_local_target(std::string_view new_target);
std::expected<void, log_errc> flush();
std::expected<void, log_errc> flush_local();
void set_global_level(log_level level);
[[nodiscard]] log_level get_global_level();
[[nodiscard]] bool is_enabled(log_level level);
[[nodiscard]] std::size_t dropped_count();

}


namespace utility::detail {

[[nodiscard]] bool should_log(log_level level) noexcept;

std::expected<void, log_errc> submit_message(
  log_level level,
  std::string&& message,
  std::source_location location);

template <typename... arg_types>
std::expected<void, log_errc> submit_log(
  log_level level,
  std::source_location location,
  std::format_string<arg_types...> message,
  arg_types&&... args) {
  if (!should_log(level)) {
    return {};
  }

  return submit_message(
      level, std::format(message, std::forward<arg_types>(args)...), location);
}

} // namespace detail

export namespace utility {

template <typename... arg_types>
std::expected<void, log_errc> log_debug(
  [[maybe_unused]] std::source_location location,
  [[maybe_unused]] std::format_string<arg_types...> message,
  [[maybe_unused]] arg_types&&... args) {
  if constexpr (debug_enabled) {
    return detail::submit_log(log_level::debug, location, message, std::forward<arg_types>(args)...);
  }
  return {};
}

template <typename... arg_types>
std::expected<void, log_errc> log_info(
  std::source_location location,
  std::format_string<arg_types...> message,
  arg_types&&... args) {
  return detail::submit_log(log_level::info, location, message, std::forward<arg_types>(args)...);
}

template <typename... arg_types>
std::expected<void, log_errc> log_warning(
  std::source_location location,
  std::format_string<arg_types...> message,
  arg_types&&... args) {
  return detail::submit_log(log_level::warning, location, message, std::forward<arg_types>(args)...);
}

template <typename... arg_types>
std::expected<void, log_errc> log_error(
  std::source_location location,
  std::format_string<arg_types...> message,
  arg_types&&... args) {
  return detail::submit_log(log_level::error, location, message, std::forward<arg_types>(args)...);
}

template <typename... arg_types>
std::expected<void, log_errc> log_fatal(
  std::source_location location,
  std::format_string<arg_types...> message,
  arg_types&&... args) {
  auto result = detail::submit_message(
      log_level::fatal, std::format(message, std::forward<arg_types>(args)...), location);
  if (!result) {
    return result;
  }
  return std::unexpected(log_errc::fatal_occurred);
}

} // namespace utility
