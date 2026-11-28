//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <list>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module scheduler;

import fiber;
import base;

export namespace pants
{
    class scheduler
    {
        public:

        using shared = std::shared_ptr<scheduler>;
        using thread_id = std::uint64_t;
        using callback = std::move_only_function<void()>;

        static constexpr thread_id any_thread = 0;

        explicit scheduler(std::size_t thread_count = 1, bool use_caller = true,
                           const std::string& name = {});
        virtual ~scheduler();

        scheduler(const scheduler&) = delete;
        scheduler& operator=(const scheduler&) = delete;
        scheduler(scheduler&&) = delete;
        scheduler& operator=(scheduler&&) = delete;

        [[nodiscard]] const std::string& name() const noexcept
        {
            return scheduler_name;
        }

        [[nodiscard]] static scheduler* get_current() noexcept;
        [[nodiscard]] static fiber* get_main_fiber() noexcept;

        void start();
        void stop();

        void add_task(fiber::shared task, thread_id id = any_thread);
        void add_task(callback entry, thread_id id = any_thread);

        template <std::ranges::input_range range>
        requires std::convertible_to<std::ranges::range_reference_t<range>, fiber::shared>
        void add_task(range&& items)
        {
            bool need_tickle = false;
            {
                std::lock_guard lock(mutex);
                for (auto&& item : items)
                {
                    need_tickle = schedule_no_lock(task{fiber::shared(item), any_thread}) || need_tickle;
                }
            }
            if (need_tickle)
            {
                tickle();
            }
        }

        template <typename callable>
        requires std::invocable<std::decay_t<callable>&> &&
                 std::constructible_from<callback, callable>
        void go(callable&& entry)
        {
            add_task(std::make_shared<fiber>(callback(std::forward<callable>(entry))));
        }

        void switch_to(thread_id id = any_thread);

        std::ostream& dump(std::ostream& output) const;

        protected:

        virtual void tickle();
        [[nodiscard]] virtual bool can_stop();
        virtual void on_thread_enter();
        virtual void idle();

        void run();
        void set_this() noexcept;

        [[nodiscard]] bool has_idle_threads() const noexcept
        {
            return idle_count.load(std::memory_order_acquire) != 0;
        }

        private:

        using work = std::variant<std::monostate, fiber::shared, callback>;

        struct task
        {
            work entry;
            thread_id id{any_thread};

            task() = default;

            task(fiber::shared value, thread_id target)
              : entry(std::move(value))
              , id(target)
            {
            }

            task(callback value, thread_id target)
              : entry(std::move(value))
              , id(target)
            {
            }

            [[nodiscard]] fiber::shared* as_fiber() noexcept
            {
                return std::get_if<fiber::shared>(&entry);
            }

            [[nodiscard]] callback* as_callback() noexcept
            {
                return std::get_if<callback>(&entry);
            }

            [[nodiscard]] bool valid() const noexcept
            {
                if (const auto* value = std::get_if<fiber::shared>(&entry))
                {
                    return static_cast<bool>(*value);
                }
                if (const auto* value = std::get_if<callback>(&entry))
                {
                    return static_cast<bool>(*value);
                }
                return false;
            }
        };

        struct fetch_result
        {
            task fetched;
            bool tickle_others{false};
            bool retry_pending{false};
        };

        [[nodiscard]] bool schedule_no_lock(task&& item);

        [[nodiscard]] fetch_result fetch_task(thread_id current_thread);

        [[nodiscard]] bool has_runnable_task_no_lock(thread_id current_thread) const;

        [[nodiscard]] bool can_stop_no_lock() const noexcept;

        void task_finished() noexcept;

        void report_fiber_failure(const fiber::shared& failed) const noexcept;

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::list<task> tasks;
        std::vector<std::thread> workers;
        fiber::shared root_fiber;
        std::string scheduler_name;
        bool use_caller_thread{false};
        bool started{false};
        std::size_t registered_workers{0};

        protected:

        std::vector<thread_id> thread_ids;
        std::size_t thread_count{0};
        std::atomic<std::size_t> active_count{0};
        std::atomic<std::size_t> idle_count{0};
        std::atomic<bool> stopping{true};
        std::atomic<bool> auto_stop{false};
        thread_id root_thread_id{any_thread};
    };

    class scheduler_switcher final
    {
        public:

        explicit scheduler_switcher(scheduler* target = nullptr);
        ~scheduler_switcher() noexcept;

        scheduler_switcher(const scheduler_switcher&) = delete;
        scheduler_switcher& operator=(const scheduler_switcher&) = delete;
        scheduler_switcher(scheduler_switcher&&) = delete;
        scheduler_switcher& operator=(scheduler_switcher&&) = delete;

        private:

        scheduler* caller{nullptr};
    };
}
