#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

import fiber;
import hook;
import iomanager;
import scheduler;
import timer;

extern "C" int connect_with_timeout(int fd, const struct sockaddr* address, socklen_t address_length,
                                    std::uint64_t timeout_ms);

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

using namespace std::chrono_literals;

} // namespace

int main() {
  pants::set_hook_enable(true);
  check(pants::is_hook_enable(), "hook should be enabled");

  int fds[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    std::puts("socketpair failed");
    return 1;
  }

  int msg_fds[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, msg_fds) != 0) {
    std::puts("socketpair failed");
    return 1;
  }

  int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in listener_address{};
  listener_address.sin_family = AF_INET;
  listener_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listener_address.sin_port = 0;
  if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&listener_address), sizeof(listener_address)) != 0
      || ::listen(listener, 4) != 0) {
    std::puts("listener setup failed");
    return 1;
  }
  socklen_t listener_length = sizeof(listener_address);
  (void)::getsockname(listener, reinterpret_cast<sockaddr*>(&listener_address), &listener_length);
  const int client = ::socket(AF_INET, SOCK_STREAM, 0);

  std::atomic<bool> read_done{false};
  std::atomic<ssize_t> read_count{-1};
  std::atomic<bool> msg_done{false};
  std::atomic<ssize_t> msg_count{-1};
  std::atomic<int> connect_result{-1};

  {
    pants::iomanager io(1, false, "hook_test");

    auto reader = std::make_shared<pants::fiber>([&] {
      char buffer[16]{};
      read_count.store(::read(fds[0], buffer, sizeof buffer), std::memory_order_release);
      read_done.store(true, std::memory_order_release);
    });
    io.add_task(reader);

    auto msg_reader = std::make_shared<pants::fiber>([&] {
      char buffer[16]{};
      iovec vector{};
      vector.iov_base = buffer;
      vector.iov_len = sizeof buffer;
      msghdr message{};
      message.msg_iov = &vector;
      message.msg_iovlen = 1;
      msg_count.store(::recvmsg(msg_fds[0], &message, 0), std::memory_order_release);
      msg_done.store(true, std::memory_order_release);
    });
    io.add_task(msg_reader);

    auto connector = std::make_shared<pants::fiber>([&] {
      connect_result.store(::connect_with_timeout(client, reinterpret_cast<const sockaddr*>(&listener_address),
                                                  sizeof(listener_address), 2000),
                           std::memory_order_release);
    });
    io.add_task(connector);

    std::this_thread::sleep_for(50ms);

    const char payload[] = "hello";
    (void)::write(fds[1], payload, sizeof(payload) - 1);

    const char msg_payload[] = "world";
    iovec send_vector{};
    send_vector.iov_base = const_cast<char*>(msg_payload);
    send_vector.iov_len = sizeof(msg_payload) - 1;
    msghdr send_message{};
    send_message.msg_iov = &send_vector;
    send_message.msg_iovlen = 1;
    (void)::sendmsg(msg_fds[1], &send_message, 0);

    int accepted = -1;
    for (int attempt = 0; attempt < 200 && accepted < 0; ++attempt) {
      accepted = ::accept(listener, nullptr, nullptr);
      if (accepted < 0) {
        std::this_thread::sleep_for(10ms);
      }
    }
    check(accepted >= 0, "listener should accept the hooked connection");
    if (accepted >= 0) {
      (void)::close(accepted);
    }

    for (int attempt = 0; attempt < 200 && !(read_done.load() && msg_done.load()); ++attempt) {
      std::this_thread::sleep_for(10ms);
    }
  }

  check(read_done.load(), "hook read should unblock after write");
  check(read_count.load() == 5, "hook read should return payload length");
  check(msg_done.load(), "hook recvmsg should unblock after sendmsg");
  check(msg_count.load() == 5, "hook recvmsg should return payload length");
  check(connect_result.load() == 0, "connect_with_timeout should connect");

  (void)::close(client);
  (void)::close(listener);
  (void)::close(fds[0]);
  (void)::close(fds[1]);
  (void)::close(msg_fds[0]);
  (void)::close(msg_fds[1]);
  pants::set_hook_enable(false);

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("hook ok");
  return 0;
}
