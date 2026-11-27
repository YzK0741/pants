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

namespace utility {

export enum class log_level : std::uint8_t {
  debug,
  info,
  warning,
  fatal,
};

export enum class log_error : std::uint8_t {
  queue_full,           // 队列达到上限，新日志被丢弃
  fatal_occurred,       // 已触发 fatal 流程，队列关闭，进程即将 abort
  target_open_failed,   // set_target 打开日志文件失败
  write_failed,         // 写目标失败
  flush_failed,         // 刷新目标失败
};

namespace detail {

[[nodiscard]] bool should_log(log_level level) noexcept;

std::expected<void, log_error> submit_message(
  log_level level,
  std::string message,
  std::source_location location);

template <typename... Args>
std::expected<void, log_error> submit_log(
  log_level level,
  const std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  if (!should_log(level)) {
    return {};
  }
  // 格式串在编译期校验，标准类型参数不会在运行时产生 format_error
  return submit_message(
      level, std::format(message, std::forward<Args>(args)...), location);
}

} // namespace detail

} // namespace utility

export namespace utility {

std::expected<void, log_error> set_target(std::string_view new_target);
std::expected<void, log_error> flush();
void set_level(log_level level);
[[nodiscard]] log_level get_level();

template <typename... Args>
std::expected<void, log_error> log_debug(
  [[maybe_unused]] const std::source_location location,
  [[maybe_unused]] std::format_string<Args...> message,
  [[maybe_unused]] Args&&... args) {
  if constexpr (!debug_enabled) {
    return {};
  } else {
    return detail::submit_log(log_level::debug, location, message, std::forward<Args>(args)...);
  }
}

template <typename... Args>
std::expected<void, log_error> log_info(
  const std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  return detail::submit_log(log_level::info, location, message, std::forward<Args>(args)...);
}

template <typename... Args>
std::expected<void, log_error> log_warning(
  const std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  return detail::submit_log(log_level::warning, location, message, std::forward<Args>(args)...);
}

template <typename... Args>
std::expected<void, log_error> log_fatal(
  const std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  auto result = detail::submit_message(
      log_level::fatal, std::format(message, std::forward<Args>(args)...), location);
  if (!result) {
    return result;
  }
  return std::unexpected<log_error>(log_error::fatal_occurred);
}

}
