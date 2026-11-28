//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <vector>
#include <shared_mutex>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utility>

module fd_manager;

namespace pants
{
    namespace
    {
        int raw_fcntl(int fd, int command, int argument)
        {
            return static_cast<int>(::syscall(SYS_fcntl, fd, command, argument));
        }
    }

    fd_ctx::fd_ctx(int descriptor)
      : descriptor(descriptor)
    {
        initialize();
    }

    void fd_ctx::initialize() noexcept
    {
        struct stat status{};
        if (initialized || descriptor < 0 || ::fstat(descriptor, &status) != 0)
        {
            initialized = false;
            return;
        }

        initialized = true;
        socket = S_ISSOCK(status.st_mode);
        if (!socket)
        {
            return;
        }

        const int flags = raw_fcntl(descriptor, F_GETFL, 0);
        if (flags < 0)
        {
            initialized = false;
            socket = false;
            return;
        }

        const bool non_blocking = (flags & O_NONBLOCK) != 0;
        user_non_block.store(non_blocking, std::memory_order_relaxed);
        if (!non_blocking && raw_fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
        {
            initialized = false;
            socket = false;
            return;
        }
        system_non_block = true;
    }

    bool fd_ctx::is_init() const noexcept
    {
        return initialized;
    }

    bool fd_ctx::is_socket() const noexcept
    {
        return socket;
    }

    bool fd_ctx::is_closed() const noexcept
    {
        return closed.load(std::memory_order_acquire);
    }

    bool fd_ctx::is_user_non_block() const noexcept
    {
        return user_non_block.load(std::memory_order_acquire);
    }

    bool fd_ctx::is_system_non_block() const noexcept
    {
        return system_non_block;
    }

    int fd_ctx::fd() const noexcept
    {
        return descriptor;
    }

    void fd_ctx::set_user_non_block(bool value) noexcept
    {
        user_non_block.store(value, std::memory_order_release);
    }

    void fd_ctx::set_timeout(int type, std::uint64_t milliseconds) noexcept
    {
        if (type == SO_RCVTIMEO)
        {
            recv_timeout.store(milliseconds, std::memory_order_release);
        }
        else if (type == SO_SNDTIMEO)
        {
            send_timeout.store(milliseconds, std::memory_order_release);
        }
    }

    std::uint64_t fd_ctx::get_timeout(int type) const noexcept
    {
        if (type == SO_RCVTIMEO)
        {
            return recv_timeout.load(std::memory_order_acquire);
        }
        if (type == SO_SNDTIMEO)
        {
            return send_timeout.load(std::memory_order_acquire);
        }
        return invalid_timeout;
    }

    void fd_ctx::mark_closed() noexcept
    {
        closed.store(true, std::memory_order_release);
    }

    fd_manager& fd_manager::instance()
    {
        static auto* manager = new fd_manager;
        return *manager;
    }

    fd_ctx::shared fd_manager::get(int fd, bool auto_create)
    {
        if (fd < 0)
        {
            return nullptr;
        }

        {
            std::shared_lock lock(mutex);
            const auto position = contexts.find(fd);
            if (position != contexts.end())
            {
                return position->second;
            }
            if (!auto_create)
            {
                return nullptr;
            }
        }

        std::unique_lock lock(mutex);
        if (const auto position = contexts.find(fd); position != contexts.end())
        {
            return position->second;
        }
        auto created = std::make_shared<fd_ctx>(fd);
        contexts.emplace(fd, created);
        return created;
    }

    void fd_manager::del(int fd)
    {
        std::unique_lock lock(mutex);
        contexts.erase(fd);
    }

    void fd_manager::close(int fd)
    {
        fd_ctx::shared context;
        {
            std::unique_lock lock(mutex);
            const auto position = contexts.find(fd);
            if (position == contexts.end())
            {
                return;
            }
            context = position->second;
            context->mark_closed();
            contexts.erase(position);
        }
    }

    std::vector<fd_ctx::shared> fd_manager::get_all() const
    {
        std::shared_lock lock(mutex);
        std::vector<fd_ctx::shared> result;
        result.reserve(contexts.size());
        for (const auto& [descriptor, context] : contexts)
        {
            (void)descriptor;
            result.push_back(context);
        }
        return result;
    }
}
