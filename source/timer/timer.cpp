//
// Created by 23530 on 2026/11/28.
//
module;

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <vector>
#include <limits>
#include <source_location>
#include <utility>

module timer;

import log;

namespace pants
{
    namespace
    {
        [[nodiscard]] timer::duration checked_duration(std::uint64_t milliseconds)
        {
            using rep = timer::duration::rep;
            if (constexpr std::uint64_t maximum = std::numeric_limits<rep>::max(); milliseconds > maximum)
            {
                utility::log_error(std::source_location::current(),
                                   "timer: interval {} ms exceeds duration range, clamped", milliseconds);
                return timer::duration(std::numeric_limits<rep>::max());
            }
            return timer::duration(static_cast<rep>(milliseconds));
        }
    }

    timer::timer(duration interval, callback entry, bool recurring, timer_manager* owner)
      : interval(interval)
      , next(clock::now() + std::chrono::duration_cast<clock::duration>(interval))
      , entry(std::move(entry))
      , owner(owner)
      , recurring(recurring)
    {
    }

    void timer::fire()
    {
        if (!entry)
        {
            return;
        }
        if (recurring)
        {
            entry();
            return;
        }
        callback pending = std::move(entry);
        entry = nullptr;
        pending();
    }

    bool timer::cancel()
    {
        timer_manager* const manager = owner.load();
        return manager != nullptr && manager->cancel(shared_from_this());
    }

    bool timer::refresh()
    {
        timer_manager* const manager = owner.load();
        return manager != nullptr && manager->refresh(shared_from_this());
    }

    bool timer::reset(duration new_interval, bool from_now)
    {
        timer_manager* const manager = owner.load();
        return manager != nullptr && manager->reset(shared_from_this(), new_interval, from_now);
    }

    bool timer::reset(std::uint64_t milliseconds, bool from_now)
    {
        return reset(checked_duration(milliseconds), from_now);
    }

    timer_manager::timer_manager() = default;

    timer_manager::~timer_manager()
    {
        cancel_all_timers();
    }

    timer::shared timer_manager::add_timer(duration interval, callback entry, bool recurring)
    {
        if (!entry || (interval <= duration::zero() && recurring))
        {
            utility::log_error(std::source_location::current(), "timer: invalid parameters when adding timer");
            return nullptr;
        }

        timer::shared added(new timer(interval, std::move(entry), recurring, this));
        bool inserted_at_front;
        {
            std::unique_lock lock(mutex);
            const auto position = timers.insert(added).first;
            inserted_at_front = position == timers.begin();
        }
        if (inserted_at_front)
        {
            on_timer_inserted_at_front();
        }
        return added;
    }

    timer::shared timer_manager::add_timer(std::uint64_t milliseconds, callback entry, bool recurring)
    {
        return add_timer(checked_duration(milliseconds), std::move(entry), recurring);
    }

    timer::shared timer_manager::add_condition_timer(duration interval, callback entry,
                                                     std::weak_ptr<void> condition, bool recurring)
    {
        if (!entry)
        {
            utility::log_error(std::source_location::current(), "timer: conditional timer callback must not be empty");
            return nullptr;
        }
        return add_timer(interval,
                         [condition = std::move(condition), entry = std::move(entry)]() mutable {
                             if (condition.lock() != nullptr)
                             {
                                 entry();
                             }
                         },
                         recurring);
    }

    timer::shared timer_manager::add_condition_timer(std::uint64_t milliseconds, callback entry,
                                                     std::weak_ptr<void> condition, bool recurring)
    {
        return add_condition_timer(checked_duration(milliseconds), std::move(entry), std::move(condition), recurring);
    }

    std::optional<timer_manager::duration> timer_manager::get_next_timeout() const
    {
        std::shared_lock lock(mutex);
        if (timers.empty())
        {
            return std::nullopt;
        }
        const clock::time_point now = clock::now();
        const clock::time_point next = (*timers.begin())->next;
        if (next <= now)
        {
            return duration::zero();
        }
        return std::chrono::ceil<duration>(next - now);
    }

    std::uint64_t timer_manager::get_next_timer() const
    {
        const std::optional<duration> timeout = get_next_timeout();
        if (!timeout)
        {
            return invalid_timer;
        }
        return static_cast<std::uint64_t>(timeout->count());
    }

    void timer_manager::list_expired_cb(std::vector<timer::shared>& expired)
    {
        const clock::time_point now = clock::now();
        std::unique_lock lock(mutex);

        auto end = timers.begin();
        while (end != timers.end() && (*end)->next <= now)
        {
            ++end;
        }
        if (end == timers.begin())
        {
            return;
        }

        std::vector<timer::shared> due(timers.begin(), end);
        timers.erase(timers.begin(), end);
        for (const timer::shared& value : due)
        {
            if (value->recurring)
            {
                value->next = now + std::chrono::duration_cast<clock::duration>(value->interval);
                timers.insert(value);
            }
            else
            {
                value->owner = nullptr;
            }
        }

        expired.reserve(expired.size() + due.size());
        for (timer::shared& value : due)
        {
            expired.push_back(std::move(value));
        }
    }

    bool timer_manager::has_timer() const
    {
        std::shared_lock lock(mutex);
        return !timers.empty();
    }

    void timer_manager::cancel_all_timers()
    {
        std::unique_lock lock(mutex);
        for (const timer::shared& entry : timers)
        {
            entry->owner = nullptr;
            entry->entry = nullptr;
        }
        timers.clear();
    }

    bool timer_manager::cancel(const timer::shared& value)
    {
        {
            std::unique_lock lock(mutex);
            if (!value || value->owner.load() != this || !value->entry)
            {
                return false;
            }
            const auto position = timers.find(value);
            if (position == timers.end())
            {
                return false;
            }
            timers.erase(position);
            value->owner = nullptr;
            timer::callback discarded = std::move(value->entry);
        }
        return true;
    }

    bool timer_manager::refresh(const timer::shared& value)
    {
        std::unique_lock lock(mutex);
        if (!value || value->owner.load() != this || !value->entry)
        {
            return false;
        }
        const auto position = timers.find(value);
        if (position == timers.end())
        {
            return false;
        }
        timers.erase(position);
        value->next = clock::now() + std::chrono::duration_cast<clock::duration>(value->interval);
        timers.insert(value);
        return true;
    }

    bool timer_manager::reset(const timer::shared& value, duration interval, bool from_now)
    {
        if (!value || (interval == duration::zero() && value->recurring))
        {
            return false;
        }

        bool inserted_at_front;
        {
            std::unique_lock lock(mutex);
            if (!value || value->owner.load() != this || !value->entry)
            {
                return false;
            }
            const auto position = timers.find(value);
            if (position == timers.end())
            {
                return false;
            }
            const clock::time_point start = from_now ? clock::now() : value->next - value->interval;
            timers.erase(position);
            value->interval = interval;
            value->next = start + std::chrono::duration_cast<clock::duration>(interval);
            const auto inserted = timers.insert(value).first;
            inserted_at_front = inserted == timers.begin();
        }
        if (inserted_at_front)
        {
            on_timer_inserted_at_front();
        }
        return true;
    }

    bool timer_manager::reset(const timer::shared& value, std::uint64_t milliseconds, bool from_now)
    {
        return reset(value, checked_duration(milliseconds), from_now);
    }
}
