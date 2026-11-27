//
// Created by 23530 on 2026/11/26.
//
module;

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <queue>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

module log:core;

import base;
import log;

namespace utility {
  namespace detail
  {

  [[nodiscard]] constexpr std::string_view file_basename(std::string_view path) noexcept {  // NOLINT(misc-use-internal-linkage)
    const auto separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
  }

  [[nodiscard]] constexpr std::string_view to_string_view(log_level level) noexcept {  // NOLINT(misc-use-internal-linkage)
    switch (level) {
      case log_level::debug:
        return "DEBUG";
      case log_level::info:
        return "INFO";
      case log_level::warning:
        return "WARNING";
      case log_level::error:
        return "ERROR";
      case log_level::fatal:
        return "FATAL";
    }
    return "UNKNOWN";
  }


  struct log_record {  // NOLINT(misc-use-internal-linkage)
    std::string message;
    std::shared_ptr<const std::string> name;
    log_level level{log_level::info};
    std::source_location location{};
    std::chrono::system_clock::time_point timestamp{};
    std::uint64_t thread_id{0};

    void write_to(std::FILE* target, std::array<char, 2048>& buffer) const {
      // 常见日志在栈缓冲区里直接格式化，超长时再退回堆上的 std::string
      const auto timestamp_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(timestamp);
      const auto level_name = to_string_view(level);
      const auto file_name = file_basename(location.file_name());
      const auto line_number = location.line();
      const auto function_name = location.function_name();
      const std::string_view name_view = name != nullptr ? std::string_view(*name) : std::string_view{};

      if (name_view.empty()) {
        const auto result = std::format_to_n(
          buffer.data(), buffer.size(),
          "[{:%Y-%m-%d %H:%M:%S}][{}][{:x}][{}:{} {}] {}\n",
          timestamp_ms, level_name, thread_id, file_name, line_number, function_name, message);
        const auto needed = static_cast<std::size_t>(result.size);
        if (needed <= buffer.size()) {
          std::fwrite(buffer.data(), 1, needed, target);
          return;
        }

        const auto line = std::format(
          "[{:%Y-%m-%d %H:%M:%S}][{}][{:x}][{}:{} {}] {}\n",
          timestamp_ms, level_name, thread_id, file_name, line_number, function_name, message);
        std::fwrite(line.data(), 1, line.size(), target);
        return;
      }

      const auto result = std::format_to_n(
        buffer.data(), buffer.size(),
        "[{:%Y-%m-%d %H:%M:%S}][{}][{}][{:x}][{}:{} {}] {}\n",
        timestamp_ms, level_name, name_view, thread_id, file_name, line_number, function_name, message);
      const auto needed = static_cast<std::size_t>(result.size);
      if (needed <= buffer.size()) {
        std::fwrite(buffer.data(), 1, needed, target);
        return;
      }

      const auto line = std::format(
        "[{:%Y-%m-%d %H:%M:%S}][{}][{}][{:x}][{}:{} {}] {}\n",
        timestamp_ms, level_name, name_view, thread_id, file_name, line_number, function_name, message);
      std::fwrite(line.data(), 1, line.size(), target);
    }
  };

  class log_writer {  // NOLINT(misc-use-internal-linkage)
  public:
    static log_writer& instance() {
      static auto* writer = new log_writer;
      return *writer;
    }

    [[nodiscard]] bool should_log(log_level level) const noexcept { // NOLINT
      if (level == log_level::fatal) {
        return true;
      }
      const auto level_value = static_cast<std::uint8_t>(level);
      return level_value >=
        static_cast<std::uint8_t>(min_level.load(std::memory_order_relaxed));
    }

    void set_level(log_level level) noexcept {
      min_level.store(level, std::memory_order_relaxed);
    }

    [[nodiscard]] log_level get_level() const noexcept {
      return min_level.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t dropped_count() const noexcept {
      return dropped.load(std::memory_order_relaxed);
    }

    std::expected<void, log_errc> submit(std::FILE* target, log_record record) {
      {
        const std::lock_guard guard(log_mutex);
        if (fatal_pending) {
          return std::unexpected(log_errc::fatal_occurred);
        }

        if (!should_log(record.level)) {
          return {};
        }

        const bool fatal = record.level == log_level::fatal;
        // fatal 记录必须入队，允许临时突破容量上限一条
        if (!fatal && log_queue.size() >= log_queue_capacity) {
          dropped.fetch_add(1, std::memory_order_relaxed);
          return std::unexpected(log_errc::queue_full);
        }

        log_queue.emplace(target, std::move(record));
        if (fatal) {
          fatal_pending = true;
        }
      }
      cv.notify_one();
      return {};
    }

    void flush() {
      std::unique_lock lock(log_mutex);
      idle_cv.wait(lock, [this] { return log_queue.empty() && !writing; });
    }

  private:
    log_writer() {
      log_thread = std::thread([this] { work(); });
      log_thread.detach();
    }

    void work() {
      std::array<char, 2048> buffer{};
      for (;;) {
        const auto item = [this] {
          std::unique_lock lock(log_mutex);
          cv.wait(lock, [this] { return !log_queue.empty(); });
          auto front = std::move(log_queue.front());
          log_queue.pop();
          writing = true;
          return front;
        }();

        item.second.write_to(item.first, buffer);

        const bool abort_now = [this] {
          const std::lock_guard guard(log_mutex);
          writing = false;
          return fatal_pending && log_queue.empty();
        }();
        idle_cv.notify_all();

        if (abort_now) {
          std::fflush(nullptr);
          std::abort();
        }
      }
    }

    std::mutex log_mutex;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::queue<std::pair<std::FILE*, log_record>> log_queue;
    std::atomic<log_level> min_level{log_level::debug};
    std::atomic<std::size_t> dropped{0};
    bool writing = false;
    bool fatal_pending = false;
    std::thread log_thread;
  };


  class local_log_info {  // NOLINT(misc-use-internal-linkage)

  public:

    static local_log_info& get_local() {
      thread_local local_log_info local;
      return local;
    }

    static std::expected<void, log_errc> change_target(std::string_view new_target) {
      const std::string path(new_target);
      std::FILE* const opened = std::fopen(path.c_str(), "ab");

      if (opened == nullptr) {
        return std::unexpected(log_errc::target_open_failed);
      }

      std::FILE* const previous = thread_info::get_local().target();
      thread_info::get_local().set_target(opened);

      if (previous != stdout) {
        log_writer::instance().flush();
        const bool write_error = std::ferror(previous) != 0;
        const int close_result = std::fclose(previous);
        if (write_error) {
          return std::unexpected(log_errc::write_failed);
        }
        if (close_result != 0) {
          return std::unexpected(log_errc::flush_failed);
        }
      }
      return {};
    }

    explicit local_log_info(std::string_view target_file = "") {
      if (!target_file.empty()) {
        (void)change_target(target_file);  // NOLINT
      }
    }

    ~local_log_info() {
      std::FILE* const target = thread_info::get_local().target();
      (void)log_writer::instance().flush();
      if (target != stdout) {
        std::fclose(target);
      } else {
        std::fflush(stdout);
      }
    }

    local_log_info(const local_log_info&) = delete;
    local_log_info& operator=(const local_log_info&) = delete;

    [[nodiscard]] static std::expected<void, log_errc> add_message(log_level level, std::string&& message, std::source_location location) {
      return log_writer::instance().submit(
        thread_info::get_local().target(),
        log_record{.message = std::move(message),
          .name = thread_info::get_local().name_holder(),
          .level = level,
          .location = location,
          .timestamp = std::chrono::system_clock::now(),
          .thread_id = thread_info::get_local().id()});
    }

    [[nodiscard]] static std::expected<void, log_errc> flush() {
      std::FILE* const target = thread_info::get_local().target();
      const int flush_result = std::fflush(target);
      if (std::ferror(target) != 0) {
        return std::unexpected(log_errc::write_failed);
      }
      if (flush_result != 0) {
        return std::unexpected(log_errc::flush_failed);
      }
      return {};
    }

  };
}

namespace detail {

bool should_log(log_level level) noexcept {
  return log_writer::instance().should_log(level);
}

std::expected<void, log_errc> submit_message(
  log_level level,
  std::string&& message,
  std::source_location location) {
  return local_log_info::get_local().add_message(level, std::move(message), location);
}

} // namespace detail

std::string_view to_string(log_level level) {
  return detail::to_string_view(level);
}

std::string_view to_string(log_errc error) {
  switch (error) {
    case log_errc::queue_full:
      return "queue_full";
    case log_errc::fatal_occurred:
      return "fatal_occurred";
    case log_errc::target_open_failed:
      return "target_open_failed";
    case log_errc::write_failed:
      return "write_failed";
    case log_errc::flush_failed:
      return "flush_failed";
  }
  return "unknown";
}

std::expected<void, log_errc> set_local_target(std::string_view new_target) {
  return detail::local_log_info::get_local().change_target(new_target);
}

std::expected<void, log_errc> flush() {
  detail::log_writer::instance().flush();
  return {};
}

std::expected<void, log_errc> flush_local() {
  return detail::local_log_info::get_local().flush();
}

void set_global_level(log_level level) {
  detail::log_writer::instance().set_level(level);
}

[[nodiscard]] log_level get_global_level() {
  return detail::log_writer::instance().get_level();
}

[[nodiscard]] bool is_enabled(log_level level) {
  return detail::log_writer::instance().should_log(level);
}

[[nodiscard]] std::size_t dropped_count() {
  return detail::log_writer::instance().dropped_count();
}

} // namespace utility