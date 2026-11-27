//
// Created by 23530 on 2026/11/27.
//
module;

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

export module base;

export namespace utility {

class thread_info {
public:
  static thread_info& get_local() noexcept {
    thread_local thread_info local;
    return local;
  }

  [[nodiscard]] std::uint64_t id() const noexcept {
    return thread_id;
  }

  void set_name(std::string_view value) {
    thread_name = std::make_shared<const std::string>(value);
  }

  [[nodiscard]] std::string_view name() const noexcept {
    return thread_name != nullptr ? std::string_view(*thread_name) : std::string_view{};
  }

  // 共享所有权：日志记录持有它，改名不会让已入队的记录悬垂
  [[nodiscard]] std::shared_ptr<const std::string> name_holder() const noexcept {
    return thread_name;
  }

  [[nodiscard]] std::FILE* target() const noexcept {
    return log_target;
  }

  void set_target(std::FILE* file) noexcept {
    log_target = file;
  }

private:
  thread_info()
    : thread_id(static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))) {
  }

  std::shared_ptr<const std::string> thread_name;
  std::uint64_t thread_id;
  std::FILE* log_target = stdout;
};

inline constexpr std::size_t log_queue_capacity = 8192;

#ifdef NDEBUG
inline constexpr bool debug_enabled = false;
#else
inline constexpr bool debug_enabled = true;
#endif

[[noreturn]] inline void panic(std::string_view message) noexcept {
  std::FILE* target = thread_info::get_local().target();
  if (target == nullptr) {
    target = stderr;
  }
  std::fwrite(message.data(), 1, message.size(), target);
  std::fputc('\n', target);
  std::fflush(target);
  std::abort();
}

}
