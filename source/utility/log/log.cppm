//
// Created by 23530 on 2026/11/26.
//
module;

#import <string_view>
#import <cstdio>
#import <chrono>
#import <thread>
#import <format>
#import <print>
#import <source_location>
#import <queue>
#import <mutex>
#import <condition_variable>
#import <string>
#import <utility>

export module utility:log;

namespace utility {

[[nodiscard]] constexpr std::string_view file_basename(std::string_view path) noexcept {
  const auto separator = path.find_last_of("/\\");
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

struct log_record {
  std::string message;
  std::source_location location{};
  std::chrono::system_clock::time_point timestamp{};

  void write_to(std::FILE* target) const {
    std::println(target, "[{:%Y-%m-%d %H:%M:%S}][{}:{}] {}",
                 std::chrono::time_point_cast<std::chrono::milliseconds>(timestamp),
                 file_basename(location.file_name()), location.line(), message);
  }
};

class log_writer {
public:
  static log_writer& instance() {
    static auto* writer = new log_writer;
    return *writer;
  }

  void submit(std::FILE* target, log_record record) {
    {
      std::lock_guard guard(log_mutex);
      log_queue.emplace(target, std::move(record));
    }
    cv.notify_one();
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
    for (;;) {
      std::pair<std::FILE*, log_record> item;
      {
        std::unique_lock lock(log_mutex);
        cv.wait(lock, [this] { return !log_queue.empty(); });
        item = std::move(log_queue.front());
        log_queue.pop();
        writing = true;
      }

      item.second.write_to(item.first);

      {
        std::lock_guard lock(log_mutex);
        writing = false;
      }
      idle_cv.notify_all();
    }
  }

  std::mutex log_mutex;
  std::condition_variable cv;
  std::condition_variable idle_cv;
  std::queue<std::pair<std::FILE*, log_record>> log_queue;
  bool writing = false;
  std::thread log_thread;
};

class log_sink {
  std::FILE* log_target = stdout;

public:

  static log_sink& get_local_sink() {
    thread_local log_sink local_sink;
    return local_sink;
  }

  void change_target(std::string_view new_target) {
    std::FILE* opened = std::fopen(new_target.data(), "ab");

    if (opened == nullptr) {
      return;
    }

    std::FILE* previous = log_target;
    log_target = opened;

    if (previous != stdout) {
      log_writer::instance().flush();
      std::fclose(previous);
    }
  }

  explicit log_sink(std::string_view target_file = "") {
    if (!target_file.empty()) {
      change_target(target_file);
    }
  }

  ~log_sink() {
    log_writer::instance().flush();
    if (log_target != stdout) {
      std::fclose(log_target);
    } else {
      std::fflush(stdout);
    }
  }

  log_sink(const log_sink&) = delete;
  log_sink& operator=(const log_sink&) = delete;

  void add_message(std::string message, const std::source_location location) const {
    log_writer::instance().submit(
        log_target,
        log_record{.message = std::move(message), .location = location, .timestamp = std::chrono::system_clock::now()});
  }

  void flush() const {
    std::fflush(log_target);
  }

};

}

export namespace utility {


void set_target(std::string_view new_target) {
  log_sink::get_local_sink().change_target(new_target);
}

// 等待队列里所有记录写出，并把本线程目标文件的缓冲刷进操作系统
void flush() {
  log_writer::instance().flush();
  log_sink::get_local_sink().flush();
}

template <typename... Args>
void log(
  const std::source_location location,
  std::format_string<Args...> message,
  Args&&... args) {
  log_sink::get_local_sink().add_message(
      std::format(message, std::forward<Args>(args)...), location);
}

}
