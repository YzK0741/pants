//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <source_location>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <utility>

module hook;

import fd_manager;
import fiber;
import iomanager;
import log;
import scheduler;
import timer;

namespace pants
{
    namespace
    {
        std::atomic<bool> hook_enabled{false};

        using sleep_fun = unsigned int (*)(unsigned int);
        using usleep_fun = int (*)(useconds_t);
        using nanosleep_fun = int (*)(const struct timespec*, struct timespec*);
        using socket_fun = int (*)(int, int, int);
        using socketpair_fun = int (*)(int, int, int, int[2]);
        using connect_fun = int (*)(int, const struct sockaddr*, socklen_t);
        using accept_fun = int (*)(int, struct sockaddr*, socklen_t*);
        using read_fun = ssize_t (*)(int, void*, std::size_t);
        using readv_fun = ssize_t (*)(int, const struct iovec*, int);
        using recv_fun = ssize_t (*)(int, void*, std::size_t, int);
        using recvfrom_fun = ssize_t (*)(int, void*, std::size_t, int, struct sockaddr*, socklen_t*);
        using write_fun = ssize_t (*)(int, const void*, std::size_t);
        using writev_fun = ssize_t (*)(int, const struct iovec*, int);
        using send_fun = ssize_t (*)(int, const void*, std::size_t, int);
        using sendto_fun = ssize_t (*)(int, const void*, std::size_t, int, const struct sockaddr*, socklen_t);
        using close_fun = int (*)(int);
        using fcntl_fun = int (*)(int, int, ...);
        using ioctl_fun = int (*)(int, unsigned long, ...);
        using getsockopt_fun = int (*)(int, int, int, void*, socklen_t*);
        using setsockopt_fun = int (*)(int, int, int, const void*, socklen_t);

        sleep_fun sleep_f = nullptr;
        usleep_fun usleep_f = nullptr;
        nanosleep_fun nanosleep_f = nullptr;
        socket_fun socket_f = nullptr;
        socketpair_fun socketpair_f = nullptr;
        connect_fun connect_f = nullptr;
        accept_fun accept_f = nullptr;
        read_fun read_f = nullptr;
        readv_fun readv_f = nullptr;
        recv_fun recv_f = nullptr;
        recvfrom_fun recvfrom_f = nullptr;
        write_fun write_f = nullptr;
        writev_fun writev_f = nullptr;
        send_fun send_f = nullptr;
        sendto_fun sendto_f = nullptr;
        close_fun close_f = nullptr;
        fcntl_fun fcntl_f = nullptr;
        ioctl_fun ioctl_f = nullptr;
        getsockopt_fun getsockopt_f = nullptr;
        setsockopt_fun setsockopt_f = nullptr;

        template <typename hook_type>
        void resolve(hook_type& slot, const char* symbol) noexcept
        {
            if (slot == nullptr)
            {
                slot = reinterpret_cast<hook_type>(::dlsym(RTLD_NEXT, symbol));
            }
        }

        void resolve_all() noexcept
        {
            resolve(sleep_f, "sleep");
            resolve(usleep_f, "usleep");
            resolve(nanosleep_f, "nanosleep");
            resolve(socket_f, "socket");
            resolve(socketpair_f, "socketpair");
            resolve(connect_f, "connect");
            resolve(accept_f, "accept");
            resolve(read_f, "read");
            resolve(readv_f, "readv");
            resolve(recv_f, "recv");
            resolve(recvfrom_f, "recvfrom");
            resolve(write_f, "write");
            resolve(writev_f, "writev");
            resolve(send_f, "send");
            resolve(sendto_f, "sendto");
            resolve(close_f, "close");
            resolve(fcntl_f, "fcntl");
            resolve(ioctl_f, "ioctl");
            resolve(getsockopt_f, "getsockopt");
            resolve(setsockopt_f, "setsockopt");
        }

        [[nodiscard]] fd_ctx::shared hook_context(int fd) noexcept
        {
            if (!hook_enabled.load(std::memory_order_relaxed))
            {
                return nullptr;
            }
            if (scheduler::get_current() == nullptr || fiber::current() == nullptr)
            {
                return nullptr;
            }
            fd_ctx::shared context = fd_manager::instance().get(fd, true);
            if (!context || !context->is_init() || context->is_closed() || context->is_user_non_block())
            {
                return nullptr;
            }
            return context;
        }

        [[nodiscard]] bool wait_ready(int fd, iomanager::event which, std::uint64_t timeout_ms) noexcept
        {
            iomanager* const io = iomanager::get_current();
            if (io == nullptr)
            {
                return false;
            }
            if (io->add_event(fd, which) != 0)
            {
                return false;
            }

            std::atomic<bool> timed_out{false};
            timer::shared timeout_timer;
            if (timeout_ms != invalid_timeout)
            {
                timeout_timer = io->add_timer(timeout_ms, [&timed_out, io, fd, which] {
                    timed_out.store(true, std::memory_order_release);
                    (void)io->cancel_event(fd, which);
                });
            }

            fiber::yield_to_hold();

            if (timeout_timer)
            {
                (void)timeout_timer->cancel();
            }
            if (timed_out.load(std::memory_order_acquire))
            {
                errno = EAGAIN;
                return false;
            }
            return true;
        }

        template <typename operation_type>
        ssize_t do_io(int fd, iomanager::event which, std::uint64_t timeout_ms, operation_type&& operation) noexcept
        {
            for (;;)
            {
                errno = 0;
                const ssize_t result = operation();
                if (result >= 0)
                {
                    return result;
                }
                if (errno == EINTR)
                {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    return result;
                }
                if (!wait_ready(fd, which, timeout_ms))
                {
                    return result;
                }
            }
        }

        [[nodiscard]] std::uint64_t timeout_of(const fd_ctx::shared& context, int type) noexcept
        {
            return context ? context->get_timeout(type) : invalid_timeout;
        }
    }

    void set_hook_enable(bool enabled) noexcept
    {
        if (enabled)
        {
            resolve_all();
        }
        hook_enabled.store(enabled, std::memory_order_release);
    }

    bool is_hook_enable() noexcept
    {
        return hook_enabled.load(std::memory_order_acquire);
    }
}

extern "C"
{
    ssize_t read(int fd, void* buffer, std::size_t count)
    {
        pants::resolve(pants::read_f, "read");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::read_f(fd, buffer, count);
        }
        return pants::do_io(fd, pants::iomanager::event::read,
                            pants::timeout_of(context, SO_RCVTIMEO),
                            [&] { return pants::read_f(fd, buffer, count); });
    }

    ssize_t write(int fd, const void* buffer, std::size_t count)
    {
        pants::resolve(pants::write_f, "write");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::write_f(fd, buffer, count);
        }
        return pants::do_io(fd, pants::iomanager::event::write,
                            pants::timeout_of(context, SO_SNDTIMEO),
                            [&] { return pants::write_f(fd, buffer, count); });
    }

    ssize_t readv(int fd, const struct iovec* vec, int count)
    {
        pants::resolve(pants::readv_f, "readv");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::readv_f(fd, vec, count);
        }
        return pants::do_io(fd, pants::iomanager::event::read,
                            pants::timeout_of(context, SO_RCVTIMEO),
                            [&] { return pants::readv_f(fd, vec, count); });
    }

    ssize_t writev(int fd, const struct iovec* vec, int count)
    {
        pants::resolve(pants::writev_f, "writev");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::writev_f(fd, vec, count);
        }
        return pants::do_io(fd, pants::iomanager::event::write,
                            pants::timeout_of(context, SO_SNDTIMEO),
                            [&] { return pants::writev_f(fd, vec, count); });
    }

    ssize_t recv(int fd, void* buffer, std::size_t length, int flags)
    {
        pants::resolve(pants::recv_f, "recv");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::recv_f(fd, buffer, length, flags);
        }
        return pants::do_io(fd, pants::iomanager::event::read,
                            pants::timeout_of(context, SO_RCVTIMEO),
                            [&] { return pants::recv_f(fd, buffer, length, flags); });
    }

    ssize_t recvfrom(int fd, void* buffer, std::size_t length, int flags,
                     struct sockaddr* address, socklen_t* address_length)
    {
        pants::resolve(pants::recvfrom_f, "recvfrom");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::recvfrom_f(fd, buffer, length, flags, address, address_length);
        }
        return pants::do_io(fd, pants::iomanager::event::read,
                            pants::timeout_of(context, SO_RCVTIMEO),
                            [&] { return pants::recvfrom_f(fd, buffer, length, flags, address, address_length); });
    }

    ssize_t send(int fd, const void* buffer, std::size_t length, int flags)
    {
        pants::resolve(pants::send_f, "send");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::send_f(fd, buffer, length, flags);
        }
        return pants::do_io(fd, pants::iomanager::event::write,
                            pants::timeout_of(context, SO_SNDTIMEO),
                            [&] { return pants::send_f(fd, buffer, length, flags); });
    }

    ssize_t sendto(int fd, const void* buffer, std::size_t length, int flags,
                   const struct sockaddr* address, socklen_t address_length)
    {
        pants::resolve(pants::sendto_f, "sendto");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::sendto_f(fd, buffer, length, flags, address, address_length);
        }
        return pants::do_io(fd, pants::iomanager::event::write,
                            pants::timeout_of(context, SO_SNDTIMEO),
                            [&] { return pants::sendto_f(fd, buffer, length, flags, address, address_length); });
    }

    int accept(int fd, struct sockaddr* address, socklen_t* address_length)
    {
        pants::resolve(pants::accept_f, "accept");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::accept_f(fd, address, address_length);
        }

        int accepted = -1;
        for (;;)
        {
            accepted = pants::accept_f(fd, address, address_length);
            if (accepted >= 0)
            {
                break;
            }
            if (errno == EINTR)
            {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                return -1;
            }
            if (!pants::wait_ready(fd, pants::iomanager::event::read,
                                   pants::timeout_of(context, SO_RCVTIMEO)))
            {
                return -1;
            }
        }

        (void)pants::fd_manager::instance().get(accepted, true);
        return accepted;
    }

    int connect(int fd, const struct sockaddr* address, socklen_t address_length)
    {
        pants::resolve(pants::connect_f, "connect");
        const pants::fd_ctx::shared context = pants::hook_context(fd);
        if (!context)
        {
            return pants::connect_f(fd, address, address_length);
        }

        const int result = pants::connect_f(fd, address, address_length);
        if (result == 0)
        {
            return 0;
        }
        if (errno != EINPROGRESS)
        {
            return result;
        }
        if (!pants::wait_ready(fd, pants::iomanager::event::write,
                               pants::timeout_of(context, SO_SNDTIMEO)))
        {
            return -1;
        }

        int error = 0;
        socklen_t error_length = sizeof(error);
        pants::resolve(pants::getsockopt_f, "getsockopt");
        if (pants::getsockopt_f(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0)
        {
            return -1;
        }
        if (error != 0)
        {
            errno = error;
            return -1;
        }
        return 0;
    }

    int close(int fd)
    {
        pants::resolve(pants::close_f, "close");
        const pants::fd_ctx::shared context = pants::fd_manager::instance().get(fd, false);
        if (context)
        {
            if (pants::iomanager* const io = pants::iomanager::get_current())
            {
                (void)io->cancel_all(fd);
            }
            pants::fd_manager::instance().close(fd);
        }
        return pants::close_f(fd);
    }

    int socket(int domain, int type, int protocol)
    {
        pants::resolve(pants::socket_f, "socket");
        const int created = pants::socket_f(domain, type, protocol);
        if (created >= 0 && pants::is_hook_enable())
        {
            (void)pants::fd_manager::instance().get(created, true);
        }
        return created;
    }

    int socketpair(int domain, int type, int protocol, int descriptors[2])
    {
        pants::resolve(pants::socketpair_f, "socketpair");
        const int result = pants::socketpair_f(domain, type, protocol, descriptors);
        if (result == 0 && pants::is_hook_enable())
        {
            (void)pants::fd_manager::instance().get(descriptors[0], true);
            (void)pants::fd_manager::instance().get(descriptors[1], true);
        }
        return result;
    }

    int fcntl(int fd, int command, ...)
    {
        pants::resolve(pants::fcntl_f, "fcntl");
        va_list arguments;
        va_start(arguments, command);
        void* argument = va_arg(arguments, void*);
        va_end(arguments);

        if (command == F_SETFL)
        {
            const pants::fd_ctx::shared context = pants::fd_manager::instance().get(fd, false);
            if (context && context->is_init() && context->is_system_non_block())
            {
                const auto flags = static_cast<int>(reinterpret_cast<intptr_t>(argument));
                context->set_user_non_block((flags & O_NONBLOCK) != 0);
                const int adjusted = (flags & ~O_NONBLOCK) | O_NONBLOCK;
                return pants::fcntl_f(fd, command, adjusted);
            }
        }
        return pants::fcntl_f(fd, command, argument);
    }

    int ioctl(int fd, unsigned long request, ...)
    {
        pants::resolve(pants::ioctl_f, "ioctl");
        va_list arguments;
        va_start(arguments, request);
        void* argument = va_arg(arguments, void*);
        va_end(arguments);

        if (request == FIONBIO)
        {
            const pants::fd_ctx::shared context = pants::fd_manager::instance().get(fd, false);
            if (context && context->is_init() && context->is_system_non_block())
            {
                const int non_block = argument != nullptr && *static_cast<int*>(argument) != 0;
                context->set_user_non_block(non_block);
            }
        }
        return pants::ioctl_f(fd, request, argument);
    }

    int setsockopt(int fd, int level, int option, const void* value, socklen_t length)
    {
        pants::resolve(pants::setsockopt_f, "setsockopt");
        if ((level == SOL_SOCKET) && (option == SO_RCVTIMEO || option == SO_SNDTIMEO) && value != nullptr)
        {
            const pants::fd_ctx::shared context = pants::fd_manager::instance().get(fd, true);
            const auto* timeout = static_cast<const struct timeval*>(value);
            if (context && timeout != nullptr)
            {
                const auto milliseconds = static_cast<std::uint64_t>(timeout->tv_sec) * 1000
                    + static_cast<std::uint64_t>(timeout->tv_usec) / 1000;
                context->set_timeout(option, milliseconds);
            }
        }
        return pants::setsockopt_f(fd, level, option, value, length);
    }

    int getsockopt(int fd, int level, int option, void* value, socklen_t* length)
    {
        pants::resolve(pants::getsockopt_f, "getsockopt");
        return pants::getsockopt_f(fd, level, option, value, length);
    }

    unsigned int sleep(unsigned int seconds)
    {
        pants::resolve(pants::sleep_f, "sleep");
        pants::iomanager* const io = pants::iomanager::get_current();
        if (!pants::is_hook_enable() || io == nullptr || pants::fiber::current() == nullptr)
        {
            return pants::sleep_f(seconds);
        }

        auto timer = io->add_timer(static_cast<std::uint64_t>(seconds) * 1000, [] {});
        if (!timer)
        {
            return pants::sleep_f(seconds);
        }
        pants::fiber::yield_to_hold();
        (void)timer->cancel();
        return 0;
    }

    int usleep(useconds_t microseconds)
    {
        pants::resolve(pants::usleep_f, "usleep");
        pants::iomanager* const io = pants::iomanager::get_current();
        if (!pants::is_hook_enable() || io == nullptr || pants::fiber::current() == nullptr)
        {
            return pants::usleep_f(microseconds);
        }

        auto timer = io->add_timer(static_cast<std::uint64_t>(microseconds) / 1000 + 1, [] {});
        if (!timer)
        {
            return pants::usleep_f(microseconds);
        }
        pants::fiber::yield_to_hold();
        (void)timer->cancel();
        return 0;
    }

    int nanosleep(const struct timespec* request, struct timespec* remaining)
    {
        pants::resolve(pants::nanosleep_f, "nanosleep");
        pants::iomanager* const io = pants::iomanager::get_current();
        if (request == nullptr || !pants::is_hook_enable() || io == nullptr || pants::fiber::current() == nullptr)
        {
            return pants::nanosleep_f(request, remaining);
        }

        const auto milliseconds = static_cast<std::uint64_t>(request->tv_sec) * 1000
            + static_cast<std::uint64_t>(request->tv_nsec) / 1000000 + 1;
        auto timer = io->add_timer(milliseconds, [] {});
        if (!timer)
        {
            return pants::nanosleep_f(request, remaining);
        }
        pants::fiber::yield_to_hold();
        (void)timer->cancel();
        return 0;
    }
}
