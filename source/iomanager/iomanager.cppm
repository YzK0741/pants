//
// Created by 23530 on 2026/11/28.
//
module;

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <variant>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

export module iomanager;

import fiber;
import scheduler;
import timer;

export namespace pants
{
    class iomanager final : public scheduler, public timer_manager
    {
        public:

        using shared = std::shared_ptr<iomanager>;
        using callback = scheduler::callback;

        enum class event : std::uint32_t
        {
            none = 0,
            read = EPOLLIN,
            write = EPOLLOUT,
        };

        explicit iomanager(std::size_t thread_count = 1, bool use_caller = true,
                           const std::string& name = {});
        ~iomanager() override;

        iomanager(const iomanager&) = delete;
        iomanager& operator=(const iomanager&) = delete;
        iomanager(iomanager&&) = delete;
        iomanager& operator=(iomanager&&) = delete;

        int add_event(int fd, event which, callback entry = {});
        bool del_event(int fd, event which);
        bool cancel_event(int fd, event which);
        bool cancel_all(int fd);

        [[nodiscard]] static iomanager* get_current() noexcept;

        protected:

        void tickle() override;
        [[nodiscard]] bool can_stop() override;
        void idle() override;
        void on_timer_inserted_at_front() override;
        void on_thread_enter() override;

        private:

        struct event_context
        {
            scheduler* owner{nullptr};
            std::variant<std::monostate, fiber::shared, callback> action;

            [[nodiscard]] bool valid() const noexcept;
            void reset() noexcept;
        };

        struct fd_context
        {
            explicit fd_context(int descriptor)
              : fd(descriptor)
            {
            }

            int fd{-1};
            std::mutex mutex;
            event_context read;
            event_context write;
            std::uint32_t events{0};

            [[nodiscard]] event_context& context(event which) noexcept
            {
                return which == event::read ? read : write;
            }
        };

        [[nodiscard]] fd_context* find_context(int fd) const;
        [[nodiscard]] fd_context* get_or_create_context(int fd);
        bool update_interest(fd_context& context, std::uint32_t new_events);
        [[nodiscard]] event_context take_event(fd_context& context, event which);
        void dispatch(event_context entry);
        void process_ready(const epoll_event& ready);
        void cancel_events();
        [[nodiscard]] bool can_stop(std::uint64_t& timeout);

        int epoll_fd{-1};
        int wake_fd{-1};
        std::atomic<std::size_t> pending_events{0};
        mutable std::shared_mutex contexts_mutex;
        std::unordered_map<int, std::unique_ptr<fd_context>> contexts;
    };
}
