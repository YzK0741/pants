#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

import fiber;
import fiber.stack;

namespace {

pants::fiber::callback make_entry(std::size_t frame) {
  if (frame >= 32768) {
    return [] {
      volatile char buffer[32768];
      for (std::size_t offset = 0; offset < sizeof(buffer); offset += 64) {
        buffer[offset] = static_cast<char>(offset);
      }
      for (;;) {
        buffer[0] = 1;
        (void)pants::fiber::yield_to_ready();
      }
    };
  }
  if (frame >= 4096) {
    return [] {
      volatile char buffer[4096];
      for (std::size_t offset = 0; offset < sizeof(buffer); offset += 64) {
        buffer[offset] = static_cast<char>(offset);
      }
      for (;;) {
        buffer[0] = 1;
        (void)pants::fiber::yield_to_ready();
      }
    };
  }
  return [] {
    for (;;) {
      (void)pants::fiber::yield_to_ready();
    }
  };
}

struct scenario {
  const char* name;
  pants::stack_mode mode;
  bool lazy;
  std::size_t frame;
  int fibers;
  int batch;
  int switches;
};

struct result {
  double create_ms{0};
  double rss_mb{0};
  double switch_us{0};
  std::size_t peak_kb{0};
};

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "benchmark failure: %s\n", message);
  std::_Exit(2);
}

void resume_checked(const std::shared_ptr<pants::fiber>& entry) {
  if (entry->resume() == pants::fiber::state::error) {
    fail("resume returned error");
  }
}

std::size_t read_rss_kb() {
  std::ifstream statm("/proc/self/statm");
  std::size_t total_pages = 0;
  std::size_t resident_pages = 0;
  statm >> total_pages >> resident_pages;
  (void)total_pages;
  return resident_pages * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
}

std::size_t read_peak_rss_kb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      std::size_t value = 0;
      std::sscanf(line.c_str() + 6, "%zu", &value);
      return value;
    }
  }
  return 0;
}

double elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::size_t display_width(std::string_view text) {
  std::size_t width = 0;
  for (std::size_t index = 0; index < text.size();) {
    const auto byte = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if ((byte & 0x80U) == 0) {
      length = 1;
    } else if ((byte & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((byte & 0xF0U) == 0xE0U) {
      length = 3;
    } else {
      length = 4;
    }
    width += length >= 3 ? 2 : 1;
    index += length;
  }
  return width;
}

std::string pad(std::string_view text, std::size_t width) {
  std::string result(text);
  const std::size_t current = display_width(text);
  if (current < width) {
    result.append(width - current, ' ');
  }
  return result;
}

result measure(const scenario& spec) {
  pants::fiber_stack_manager::instance().set_lazy_slots(spec.lazy);

  const std::size_t rss_before = read_rss_kb();

  pants::fiber::option options{};
  options.stack_mode = spec.mode;
  options.stack_size = spec.frame >= 32768 ? 64 * 1024 : 32 * 1024;

  std::vector<std::shared_ptr<pants::fiber>> entries;
  entries.reserve(static_cast<std::size_t>(spec.fibers));

  const auto create_start = std::chrono::steady_clock::now();
  for (int index = 0; index < spec.fibers; ++index) {
    entries.push_back(std::make_shared<pants::fiber>(make_entry(spec.frame), options));
  }
  for (std::shared_ptr<pants::fiber>& entry : entries) {
    resume_checked(entry);
  }
  const auto create_end = std::chrono::steady_clock::now();

  const std::size_t rss_after = read_rss_kb();

  const int active = spec.batch < spec.fibers ? spec.batch : spec.fibers;
  for (int index = 0; index < active; ++index) {
    resume_checked(entries[static_cast<std::size_t>(index)]);
  }
  const int rounds = spec.switches / active;
  const auto switch_start = std::chrono::steady_clock::now();
  for (int round = 0; round < rounds; ++round) {
    for (int index = 0; index < active; ++index) {
      resume_checked(entries[static_cast<std::size_t>(index)]);
    }
  }
  const auto switch_end = std::chrono::steady_clock::now();
  const int resumes = rounds * active;

  result measured;
  measured.create_ms = elapsed_ms(create_start, create_end);
  measured.rss_mb = (static_cast<double>(rss_after) - static_cast<double>(rss_before)) / 1024.0;
  measured.switch_us = elapsed_ms(switch_start, switch_end) * 1000.0 / resumes;
  measured.peak_kb = read_peak_rss_kb();
  return measured;
}

bool run_isolated(const scenario& spec, result& measured) {
  int pipe_fds[2]{};
  if (::pipe(pipe_fds) != 0) {
    return false;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    ::close(pipe_fds[0]);
    const result child_result = measure(spec);
    char buffer[256]{};
    const int length = std::snprintf(buffer, sizeof(buffer), "%.3f %.3f %.3f %zu\n",
                                     child_result.create_ms, child_result.rss_mb,
                                     child_result.switch_us, child_result.peak_kb);
    (void)::write(pipe_fds[1], buffer, static_cast<std::size_t>(length));
    ::close(pipe_fds[1]);
    std::_Exit(0);
  }

  ::close(pipe_fds[1]);
  std::string output;
  char chunk[256]{};
  ssize_t read_count = 0;
  while ((read_count = ::read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
    output.append(chunk, static_cast<std::size_t>(read_count));
  }
  ::close(pipe_fds[0]);

  int status = 0;
  (void)::waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return false;
  }
  return std::sscanf(output.c_str(), "%lf %lf %lf %zu",
                     &measured.create_ms, &measured.rss_mb, &measured.switch_us,
                     &measured.peak_kb) == 4;
}

void print_row(const char* load, std::size_t frame, int fibers, int batch, int switches) {
  const scenario dedicated{load, pants::stack_mode::dedicated, false, frame, fibers, batch, switches};
  const scenario eager{load, pants::stack_mode::shared, false, frame, fibers, batch, switches};
  const scenario lazy{load, pants::stack_mode::shared, true, frame, fibers, batch, switches};

  result dedicated_result;
  result eager_result;
  result lazy_result;
  if (!run_isolated(dedicated, dedicated_result)
      || !run_isolated(eager, eager_result)
      || !run_isolated(lazy, lazy_result)) {
    std::printf("%s%s\n", pad(load, 30).c_str(), "测量失败");
    return;
  }

  std::printf("%s%14.2fms/%.1fMB%14.2fms/%.1fMB%14.2fms/%.1fMB\n",
              pad(load, 30).c_str(),
              dedicated_result.create_ms, dedicated_result.rss_mb,
              eager_result.create_ms, eager_result.rss_mb,
              lazy_result.create_ms, lazy_result.rss_mb);
}

void print_switch_row(const char* load, std::size_t frame, int fibers, int batch, int switches) {
  const scenario dedicated{load, pants::stack_mode::dedicated, false, frame, fibers, batch, switches};
  const scenario eager{load, pants::stack_mode::shared, false, frame, fibers, batch, switches};
  const scenario lazy{load, pants::stack_mode::shared, true, frame, fibers, batch, switches};

  result dedicated_result;
  result eager_result;
  result lazy_result;
  if (!run_isolated(dedicated, dedicated_result)
      || !run_isolated(eager, eager_result)
      || !run_isolated(lazy, lazy_result)) {
    std::printf("%s%s\n", pad(load, 30).c_str(), "测量失败");
    return;
  }

  std::printf("%s%16.3fus%17.3fus%17.3fus\n",
              pad(load, 30).c_str(),
              dedicated_result.switch_us, eager_result.switch_us, lazy_result.switch_us);
}

} // namespace

int main() {
  constexpr int fibers = 20000;
  constexpr int switches = 64000;

  std::printf("%s%s%s%s\n",
              pad("负载", 30).c_str(),
              pad("独占栈", 20).c_str(),
              pad("立即归还共享栈", 20).c_str(),
              pad("懒归还共享栈", 20).c_str());

  print_row("轻栈/20000", 0, fibers, 32, switches);
  print_row("4K栈/20000", 4096, fibers, 32, switches);
  print_row("32K栈/20000", 32768, fibers, 32, switches);

  std::printf("\n%s%s%s%s\n",
              pad("单次切换耗时", 30).c_str(),
              pad("独占栈", 20).c_str(),
              pad("立即归还共享栈", 20).c_str(),
              pad("懒归还共享栈", 20).c_str());

  print_switch_row("轻栈/20000", 0, fibers, 32, switches);
  print_switch_row("4K栈/20000", 4096, fibers, 32, switches);
  print_switch_row("32K栈/20000", 32768, fibers, 32, switches);
  print_switch_row("32K栈/20000·96轮转", 32768, fibers, 96, switches);
  return 0;
}
