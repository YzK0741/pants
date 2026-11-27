//
// Created by 23530 on 2026/11/26.
//
module;

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <mutex>
#include <queue>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

module utility:log_detail;

import :build;
import :log;

namespace utility {

[[nodiscard]] constexpr std::string_view file_basename(std::string_view path) noexcept {
  const auto separator = path.find_last_of("/\\");
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

[[nodiscard]] constexpr std::string_view to_string_view(log_level level) noexcept {
  switch (level) {
    case log_level::debug:
      return "DEBUG";
    case log_level::info:
      return "INFO";
    case log_level::warning:
      return "WARNING";
    case log_level::fatal:
      return "FATAL";
  }
  return "UNKNOWN";
}

struct log_record {
  std::string message;
  log_level level{log_level::info};
  std::source_location location{};
  std::chrono::system_clock::time_point timestamp{};

  void write_to(std::FILE* target, std::array<char, 2048>& buffer) const {
    // 常见日志在栈缓冲区里直接格式化，超长时再退回堆上的 std::string
    const auto result = std::format_to_n(
        buffer.data(), buffer.size(),
        "[{:%Y-%m-%d %H:%M:%S}][{}][{}:{} {}] {}\n",
        std::chrono::time_point_cast<std::chrono::milliseconds>(timestamp),
        to_string_view(level),
        file_basename(location.file_name()), location.line(),
        location.function_name(), message);
    const auto needed = static_cast<std::size_t>(result.size);
    if (needed <= buffer.size()) {
      std::fwrite(buffer.data(), 1, needed, target);
      return;
    }

    const auto line = std::format(
        "[{:%Y-%m-%d %H:%M:%S}][{}][{}:{} {}] {}\n",
        std::chrono::time_point_cast<std::chrono::milliseconds>(timestamp),
        to_string_view(level),
        file_basename(location.file_name()), location.line(),
        location.function_name(), message);
    std::fwrite(line.data(), 1, line.size(), target);
  }
};

class log_writer {
public:
  static log_writer& instance() {
    static auto* writer = new log_writer;
    return *writer;
  }

  [[nodiscard]] bool should_log(log_level level) const noexcept {
    if (level == log_level::fatal) {
      return true;
    }
    return static_cast<std::uint8_t>(level) >=
           static_cast<std::uint8_t>(min_level.load(std::memory_order_relaxed));
  }

  void set_level(log_level level) noexcept {
    min_level.store(level, std::memory_order_relaxed);
  }

  [[nodiscard]] log_level get_level() const noexcept {
    return min_level.load(std::memory_order_relaxed);
  }

  std::expected<void, log_error> submit(std::FILE* target, log_record record) {
    {
      std::lock_guard guard(log_mutex);
      if (fatal_pending) {
        return std::unexpected(log_error::fatal_occurred);
      }

      if (!should_log(record.level)) {
        return {};
      }

      const bool fatal = record.level == log_level::fatal;
      // fatal 记录必须入队，允许临时突破容量上限一条
      if (!fatal && log_queue.size() >= log_queue_capacity) {
        return std::unexpected(log_error::queue_full);
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
      std::pair<std::FILE*, log_record> item;
      {
        std::unique_lock lock(log_mutex);
        cv.wait(lock, [this] { return !log_queue.empty(); });
        item = std::move(log_queue.front());
        log_queue.pop();
        writing = true;
      }

      item.second.write_to(item.first, buffer);

      bool abort_now = false;
      {
        std::lock_guard lock(log_mutex);
        writing = false;
        abort_now = fatal_pending && log_queue.empty();
      }
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
  bool writing = false;
  bool fatal_pending = false;
  std::thread log_thread;
};

class log_sink {
  std::FILE* log_target = stdout;

public:

  static log_sink& get_local_sink() {
    thread_local log_sink local_sink;
    return local_sink;
  }

  std::expected<void, log_error> change_target(std::string_view new_target) {
    const std::string path(new_target);
    std::FILE* opened = std::fopen(path.c_str(), "ab");

    if (opened == nullptr) {
      return std::unexpected(log_error::target_open_failed);
    }

    std::FILE* previous = log_target;
    log_target = opened;

    if (previous != stdout) {
      log_writer::instance().flush();
      const bool write_error = std::ferror(previous) != 0;
      const int close_result = std::fclose(previous);
      if (write_error) {
        return std::unexpected(log_error::write_failed);
      }
      if (close_result != 0) {
        return std::unexpected(log_error::flush_failed);
      }
    }
    return {};
  }

  explicit log_sink(std::string_view target_file = "") {
    if (!target_file.empty()) {
      (void)change_target(target_file);
    }
  }

  ~log_sink() {
    (void)log_writer::instance().flush();
    if (log_target != stdout) {
      std::fclose(log_target);
    } else {
      std::fflush(stdout);
    }
  }

  log_sink(const log_sink&) = delete;
  log_sink& operator=(const log_sink&) = delete;

  std::expected<void, log_error> add_message(log_level level, std::string message, const std::source_location location) const {
    return log_writer::instance().submit(
        log_target,
        log_record{.message = std::move(message),
                   .level = level,
                   .location = location,
                   .timestamp = std::chrono::system_clock::now()});
  }

  std::expected<void, log_error> flush() const {
    const int flush_result = std::fflush(log_target);
    if (std::ferror(log_target) != 0) {
      return std::unexpected(log_error::write_failed);
    }
    if (flush_result != 0) {
      return std::unexpected(log_error::flush_failed);
    }
    return {};
  }

};

namespace detail {

bool should_log(log_level level) noexcept {
  return log_writer::instance().should_log(level);
}

std::expected<void, log_error> submit_message(
  log_level level,
  std::string message,
  const std::source_location location) {
  return log_sink::get_local_sink().add_message(level, std::move(message), location);
}

} // namespace detail

std::expected<void, log_error> set_target(std::string_view new_target) {
  return log_sink::get_local_sink().change_target(new_target);
}

// 等待队列里所有记录写出，并把本线程目标文件的缓冲刷进操作系统
std::expected<void, log_error> flush() {
  log_writer::instance().flush();
  return log_sink::get_local_sink().flush();
}

void set_level(log_level level) {
  log_writer::instance().set_level(level);
}

[[nodiscard]] log_level get_level() {
  return log_writer::instance().get_level();
}

}
