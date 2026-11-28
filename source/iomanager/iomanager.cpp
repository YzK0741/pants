//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <utility>
#include <variant>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

module iomanager;

import fiber;
import scheduler;
import timer;
import log;

namespace pants
{
    namespace
    {
        constexpr int max_epoll_timeout_ms = 3000;
        constexpr std::size_t max_epoll_events = 256;
        thread_local iomanager* current_io = nullptr;

        void log_epoll_failure(const char* operation, int fd) noexcept
        {
            utility::log_error(std::source_location::current(), "iomanager: {} fd={} failed: {}",
                               operation, fd, std::strerror(errno));
        }
    }

    bool iomanager::event_context::valid() const noexcept
    {
        if (owner == nullptr)
        {
            return false;
        }
        if (const auto* fiber_entry = std::get_if<fiber::shared>(&action))
        {
            return static_cast<bool>(*fiber_entry);
        }
        if (const auto* callback_entry = std::get_if<callback>(&action))
        {
            return static_cast<bool>(*callback_entry);
        }
        return false;
    }

    void iomanager::event_context::reset() noexcept
    {
        owner = nullptr;
        action.emplace<std::monostate>();
    }

    iomanager::iomanager(std::size_t thread_count, bool use_caller, const std::string& name)
      : scheduler(thread_count, use_caller, name)
    {
        epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0)
        {
            utility::log_fatal(std::source_location::current(), "iomanager: epoll_create1 failed");
        }

        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0)
        {
            utility::log_fatal(std::source_location::current(), "iomanager: eventfd failed");
        }

        epoll_event wake_event{};
        wake_event.events = EPOLLIN;
        wake_event.data.fd = wake_fd;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &wake_event) != 0)
        {
            utility::log_fatal(std::source_location::current(), "iomanager: cannot register wake fd");
        }

        current_io = this;
        start();
    }

    iomanager::~iomanager()
    {
        cancel_all_timers();
        cancel_events();
        scheduler::stop();

        if (wake_fd >= 0)
        {
            (void)::close(wake_fd);
            wake_fd = -1;
        }
        if (epoll_fd >= 0)
        {
            (void)::close(epoll_fd);
            epoll_fd = -1;
        }
        if (current_io == this)
        {
            current_io = nullptr;
        }
    }

    iomanager* iomanager::get_current() noexcept
    {
        return current_io;
    }

    void iomanager::on_thread_enter()
    {
        current_io = this;
    }

    void iomanager::tickle()
    {
        constexpr std::uint64_t one = 1;
        ssize_t result;
        do
        {
            result = ::write(wake_fd, &one, sizeof(one));
        } while (result < 0 && errno == EINTR);

        if (result < 0 && errno != EAGAIN)
        {
            log_epoll_failure("eventfd write", wake_fd);
        }
    }

    void iomanager::on_timer_inserted_at_front()
    {
        tickle();
    }

    bool iomanager::can_stop(std::uint64_t& timeout)
    {
        timeout = get_next_timer();
        return timeout == invalid_timer
            && pending_events.load(std::memory_order_acquire) == 0
            && scheduler::can_stop();
    }

    bool iomanager::can_stop()
    {
        std::uint64_t timeout = invalid_timer;
        return can_stop(timeout);
    }

    void iomanager::idle()
    {
        std::array<epoll_event, max_epoll_events> events{};
        for (;;)
        {
            int ready_count;
            do
            {
                std::uint64_t next_timeout = invalid_timer;
                if (can_stop(next_timeout))
                {
                    return;
                }
                const int timeout = next_timeout == invalid_timer
                    ? max_epoll_timeout_ms
                    : static_cast<int>(std::min<std::uint64_t>(next_timeout, max_epoll_timeout_ms));
                ready_count = ::epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), timeout);
            } while (ready_count < 0 && errno == EINTR);

            std::vector<timer::shared> expired;
            list_expired_cb(expired);
            for (timer::shared& value : expired)
            {
                add_task(callback([holder = std::move(value)]() mutable { holder->fire(); }));
            }

            for (int index = 0; index < ready_count; ++index)
            {
                process_ready(events[static_cast<std::size_t>(index)]);
            }

            fiber::yield_to_hold();
        }
    }

    iomanager::fd_context* iomanager::find_context(int fd) const
    {
        std::shared_lock lock(contexts_mutex);
        const auto position = contexts.find(fd);
        return position == contexts.end() ? nullptr : position->second.get();
    }

    iomanager::fd_context* iomanager::get_or_create_context(int fd)
    {
        {
            std::shared_lock lock(contexts_mutex);
            const auto position = contexts.find(fd);
            if (position != contexts.end())
            {
                return position->second.get();
            }
        }
        std::unique_lock lock(contexts_mutex);
        auto [position, inserted] = contexts.try_emplace(fd);
        if (inserted)
        {
            position->second = std::make_unique<fd_context>(fd);
        }
        return position->second.get();
    }

    bool iomanager::update_interest(fd_context& context, std::uint32_t new_events)
    {
        const int operation = context.events == 0
            ? EPOLL_CTL_ADD
            : (new_events == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD);

        epoll_event event{};
        event.events = 0;
        if ((new_events & static_cast<std::uint32_t>(event::read)) != 0)
        {
            event.events |= EPOLLIN;
        }
        if ((new_events & static_cast<std::uint32_t>(event::write)) != 0)
        {
            event.events |= EPOLLOUT;
        }
        event.data.fd = context.fd;

        if (::epoll_ctl(epoll_fd, operation, context.fd, &event) != 0)
        {
            if (new_events == 0 && (errno == EBADF || errno == ENOENT))
            {
                context.events = 0;
                return true;
            }
            log_epoll_failure("epoll_ctl", context.fd);
            return false;
        }

        context.events = new_events;
        return true;
    }

    iomanager::event_context iomanager::take_event(fd_context& context, event which)
    {
        event_context result = std::move(context.context(which));
        context.context(which).reset();
        return result;
    }

    int iomanager::add_event(int fd, event which, callback entry)
    {
        if (fd < 0 || (which != event::read && which != event::write))
        {
            return -1;
        }

        event_context new_context;
        new_context.owner = scheduler::get_current();
        if (entry)
        {
            new_context.action.emplace<callback>(std::move(entry));
            if (new_context.owner == nullptr)
            {
                new_context.owner = this;
            }
        }
        else
        {
            fiber::shared current = fiber::shared_current();
            if (new_context.owner == nullptr || !current)
            {
                utility::log_error(std::source_location::current(),
                                   "iomanager: add_event without callback must be called inside a fiber");
                return -1;
            }
            new_context.action.emplace<fiber::shared>(std::move(current));
        }

        fd_context* context = get_or_create_context(fd);
        std::lock_guard lock(context->mutex);
        if ((context->events & static_cast<std::uint32_t>(which)) != 0)
        {
            utility::log_error(std::source_location::current(),
                               "iomanager: add_event fd={} duplicated event", fd);
            return -1;
        }

        const std::uint32_t updated = context->events | static_cast<std::uint32_t>(which);
        if (!update_interest(*context, updated))
        {
            return -2;
        }

        context->context(which) = std::move(new_context);
        pending_events.fetch_add(1, std::memory_order_acq_rel);
        return 0;
    }

    bool iomanager::del_event(int fd, event which)
    {
        fd_context* context = find_context(fd);
        if (context == nullptr)
        {
            return false;
        }


        {
            std::lock_guard lock(context->mutex);
            const std::uint32_t mask = static_cast<std::uint32_t>(which);
            if ((context->events & mask) == 0)
            {
                return false;
            }
            if (!update_interest(*context, context->events & ~mask))
            {
                return false;
            }
            (void)take_event(*context, which);
        }
        pending_events.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    bool iomanager::cancel_event(int fd, event which)
    {
        fd_context* context = find_context(fd);
        if (context == nullptr)
        {
            return false;
        }

        event_context canceled;
        {
            std::lock_guard lock(context->mutex);
            const std::uint32_t mask = static_cast<std::uint32_t>(which);
            if ((context->events & mask) == 0)
            {
                return false;
            }
            if (!update_interest(*context, context->events & ~mask))
            {
                return false;
            }
            canceled = take_event(*context, which);
        }
        pending_events.fetch_sub(1, std::memory_order_acq_rel);
        dispatch(std::move(canceled));
        return true;
    }

    bool iomanager::cancel_all(int fd)
    {
        fd_context* context = find_context(fd);
        if (context == nullptr)
        {
            return false;
        }

        event_context read_entry;
        event_context write_entry;
        std::size_t canceled = 0;
        {
            std::lock_guard lock(context->mutex);
            if (context->events == 0)
            {
                return false;
            }
            const std::uint32_t old_events = context->events;
            if (!update_interest(*context, 0))
            {
                return false;
            }
            if ((old_events & static_cast<std::uint32_t>(event::read)) != 0)
            {
                read_entry = take_event(*context, event::read);
                ++canceled;
            }
            if ((old_events & static_cast<std::uint32_t>(event::write)) != 0)
            {
                write_entry = take_event(*context, event::write);
                ++canceled;
            }
        }

        pending_events.fetch_sub(canceled, std::memory_order_acq_rel);
        dispatch(std::move(read_entry));
        dispatch(std::move(write_entry));
        return true;
    }

    void iomanager::cancel_events()
    {
        std::vector<event_context> canceled;
        std::size_t removed = 0;

        std::unique_lock contexts_lock(contexts_mutex);
        for (auto& [descriptor, holder] : contexts)
        {
            (void)descriptor;
            fd_context& context = *holder;
            std::lock_guard lock(context.mutex);
            if (context.events == 0)
            {
                continue;
            }
            epoll_event ignored{};
            (void)::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, context.fd, &ignored);
            if ((context.events & static_cast<std::uint32_t>(event::read)) != 0)
            {
                canceled.push_back(take_event(context, event::read));
                ++removed;
            }
            if ((context.events & static_cast<std::uint32_t>(event::write)) != 0)
            {
                canceled.push_back(take_event(context, event::write));
                ++removed;
            }
            context.events = 0;
        }
        contexts_lock.unlock();

        if (removed != 0)
        {
            pending_events.fetch_sub(removed, std::memory_order_acq_rel);
        }
        for (event_context& entry : canceled)
        {
            dispatch(std::move(entry));
        }
    }

    void iomanager::dispatch(event_context entry)
    {
        if (!entry.valid())
        {
            return;
        }

        if (auto* callback_entry = std::get_if<callback>(&entry.action))
        {
            entry.owner->add_task(std::move(*callback_entry));
            return;
        }

        if (auto* fiber_entry = std::get_if<fiber::shared>(&entry.action))
        {
            if ((*fiber_entry)->get_state() == fiber::state::waiting)
            {
                (void)(*fiber_entry)->wake();
            }
            entry.owner->add_task(std::move(*fiber_entry));
        }
    }

    void iomanager::process_ready(const epoll_event& ready)
    {
        if (ready.data.fd == wake_fd)
        {
            std::uint64_t value = 0;
            while (::read(wake_fd, &value, sizeof(value)) < 0 && errno == EINTR)
            {
            }
            return;
        }

        fd_context* context = find_context(ready.data.fd);
        if (context == nullptr)
        {
            return;
        }

        event_context read_entry;
        event_context write_entry;
        std::size_t triggered = 0;
        {
            std::lock_guard lock(context->mutex);
            if (context->events == 0)
            {
                return;
            }

            std::uint32_t ready_bits = ready.events;
            if ((ready_bits & (EPOLLERR | EPOLLHUP)) != 0)
            {
                ready_bits |= (EPOLLIN | EPOLLOUT) & context->events;
            }

            const std::uint32_t fired = ready_bits & (EPOLLIN | EPOLLOUT) & context->events;
            if (fired == 0)
            {
                return;
            }

            (void)update_interest(*context, context->events & ~fired);

            if ((fired & static_cast<std::uint32_t>(event::read)) != 0)
            {
                read_entry = take_event(*context, event::read);
                ++triggered;
            }
            if ((fired & static_cast<std::uint32_t>(event::write)) != 0)
            {
                write_entry = take_event(*context, event::write);
                ++triggered;
            }
        }

        pending_events.fetch_sub(triggered, std::memory_order_acq_rel);
        dispatch(std::move(read_entry));
        dispatch(std::move(write_entry));
    }

    iomanager::fd_context::fd_context(int descriptor)
      : fd(descriptor)
    {
    }

    iomanager::event_context& iomanager::fd_context::context(event which) noexcept
    {
        return which == event::read ? read : write;
    }
}
