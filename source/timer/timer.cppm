//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <vector>

export module timer;

namespace pants
{
    export class timer_manager;

    export class timer final : public std::enable_shared_from_this<timer>
    {
        public:

        using shared = std::shared_ptr<timer>;
        using callback = std::move_only_function<void()>;
        using clock = std::chrono::steady_clock;
        using duration = std::chrono::milliseconds;

        struct comparator
        {
            bool operator()(const shared& lhs, const shared& rhs) const noexcept
            {
                if (lhs == rhs)
                {
                    return false;
                }
                if (!lhs)
                {
                    return true;
                }
                if (!rhs)
                {
                    return false;
                }
                if (lhs->next != rhs->next)
                {
                    return lhs->next < rhs->next;
                }
                return lhs.get() < rhs.get();
            }
        };

        void fire();
        bool cancel();
        bool refresh();
        bool reset(duration interval, bool from_now);
        bool reset(std::uint64_t milliseconds, bool from_now);

        private:

        friend class timer_manager;

        timer(duration interval, callback entry, bool recurring, timer_manager* owner);

        duration interval{duration::zero()};
        clock::time_point next{};
        callback entry{};
        std::atomic<timer_manager*> owner{nullptr};
        bool recurring{false};
    };

    class timer_manager
    {
        public:

        using clock = timer::clock;
        using duration = timer::duration;
        using callback = timer::callback;

        static constexpr std::uint64_t invalid_timer = std::numeric_limits<std::uint64_t>::max();

        timer_manager();
        virtual ~timer_manager();

        timer_manager(const timer_manager&) = delete;
        timer_manager& operator=(const timer_manager&) = delete;
        timer_manager(timer_manager&&) = delete;
        timer_manager& operator=(timer_manager&&) = delete;

        timer::shared add_timer(duration interval, callback entry, bool recurring = false);
        timer::shared add_timer(std::uint64_t milliseconds, callback entry, bool recurring = false);
        timer::shared add_condition_timer(duration interval, callback entry,
                                          std::weak_ptr<void> condition, bool recurring = false);
        timer::shared add_condition_timer(std::uint64_t milliseconds, callback entry,
                                          std::weak_ptr<void> condition, bool recurring = false);

        [[nodiscard]] std::optional<duration> get_next_timeout() const;
        [[nodiscard]] std::uint64_t get_next_timer() const;
        void list_expired_cb(std::vector<timer::shared>& expired);
        [[nodiscard]] bool has_timer() const;
        void cancel_all_timers();

        protected:

        virtual void on_timer_inserted_at_front() = 0;

        private:

        friend class timer;

        bool cancel(const timer::shared& value);
        bool refresh(const timer::shared& value);
        bool reset(const timer::shared& value, duration interval, bool from_now);
        bool reset(const timer::shared& value, std::uint64_t milliseconds, bool from_now);

        mutable std::shared_mutex mutex;
        std::set<timer::shared, timer::comparator> timers;
    };
}
