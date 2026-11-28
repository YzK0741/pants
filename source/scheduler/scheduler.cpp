//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <cstddef>
#include <source_location>
#include <string>
#include <utility>

module scheduler;

import fiber;
import base;
import log;

namespace pants
{
    namespace
    {
        thread_local scheduler* current_scheduler = nullptr;
        thread_local fiber* current_scheduler_fiber = nullptr;

        [[nodiscard]] std::string worker_name(const std::string& scheduler_name, std::size_t index)
        {
            const std::string base = scheduler_name.empty() ? "scheduler" : scheduler_name;
            return base + "_" + std::to_string(index);
        }
    }

    scheduler::scheduler(std::size_t thread_count, bool use_caller, const std::string& name)
      : scheduler_name(name)
      , use_caller_thread(use_caller)
    {
        if (thread_count == 0)
        {
            utility::log_fatal(std::source_location::current(), "scheduler requires at least one thread");
        }

        if (use_caller_thread)
        {
            if (current_scheduler != nullptr)
            {
                utility::log_fatal(std::source_location::current(),
                                   "the current thread already owns a caller scheduler");
            }
            --thread_count;
            root_thread_id = utility::thread_info::get_local().id();
            thread_ids.push_back(root_thread_id);
            root_fiber = std::make_shared<fiber>([this] { run(); });
            current_scheduler = this;
            current_scheduler_fiber = root_fiber.get();
            if (!scheduler_name.empty())
            {
                utility::thread_info::get_local().set_name(scheduler_name);
            }
        }

        this->thread_count = thread_count;
    }

    scheduler::~scheduler()
    {
        stop();
        if (current_scheduler == this)
        {
            current_scheduler = nullptr;
            current_scheduler_fiber = nullptr;
        }
    }

    scheduler* scheduler::get_current() noexcept
    {
        return current_scheduler;
    }

    fiber* scheduler::get_main_fiber() noexcept
    {
        return current_scheduler_fiber;
    }

    void scheduler::set_this() noexcept
    {
        current_scheduler = this;
    }

    bool scheduler::schedule_no_lock(task&& item)
    {
        if (!item.valid())
        {
            return false;
        }
        const bool need_tickle = tasks.empty() || has_idle_threads();
        tasks.emplace_back(std::move(item));
        return need_tickle;
    }

    void scheduler::add_task(fiber::shared task_fiber, thread_id id)
    {
        bool need_tickle = false;
        {
            std::lock_guard lock(mutex);
            need_tickle = schedule_no_lock(task{std::move(task_fiber), id});
        }
        if (need_tickle)
        {
            tickle();
        }
    }

    void scheduler::add_task(callback entry, thread_id id)
    {
        bool need_tickle = false;
        {
            std::lock_guard lock(mutex);
            need_tickle = schedule_no_lock(task{std::move(entry), id});
        }
        if (need_tickle)
        {
            tickle();
        }
    }

    void scheduler::on_thread_enter()
    {
    }

    void scheduler::start()
    {
        std::unique_lock lock(mutex);
        if (started)
        {
            return;
        }
        if (root_fiber)
        {
            const fiber::state state = root_fiber->get_state();
            if (state == fiber::state::terminated || state == fiber::state::error)
            {
                root_fiber->reset([this] { run(); });
            }
            else if (state != fiber::state::init)
            {
                utility::log_fatal(std::source_location::current(),
                                   "caller scheduler root fiber is still active");
            }
        }

        auto_stop.store(false, std::memory_order_release);
        stopping.store(false, std::memory_order_release);
        registered_workers = 0;
        started = true;
        workers.clear();
        workers.reserve(thread_count);

        for (std::size_t index = 0; index < thread_count; ++index)
        {
            workers.emplace_back([this, index] {
                utility::thread_info::get_local().set_name(worker_name(scheduler_name, index));
                run();
            });
        }
        cv.wait(lock, [this] { return registered_workers == thread_count; });
    }

    scheduler::fetch_result scheduler::fetch_task(thread_id current_thread)
    {
        fetch_result result;
        std::lock_guard lock(mutex);

        for (auto iterator = tasks.begin(); iterator != tasks.end();)
        {
            if (!iterator->valid())
            {
                iterator = tasks.erase(iterator);
                continue;
            }
            if (iterator->id != any_thread && iterator->id != current_thread)
            {
                result.tickle_others = true;
                ++iterator;
                continue;
            }
            if (fiber::shared* scheduled = iterator->as_fiber())
            {
                fiber::state state = (*scheduled)->get_state();
                if (state == fiber::state::terminated || state == fiber::state::error)
                {
                    iterator = tasks.erase(iterator);
                    continue;
                }
                if (state == fiber::state::executing)
                {
                    result.retry_pending = true;
                    ++iterator;
                    continue;
                }
                if (state == fiber::state::waiting)
                {
                    if (!(*scheduled)->wake())
                    {
                        result.retry_pending = true;
                        ++iterator;
                        continue;
                    }
                    state = (*scheduled)->get_state();
                }
                if (state != fiber::state::init && state != fiber::state::ready)
                {
                    ++iterator;
                    continue;
                }
            }
            result.fetched = std::move(*iterator);
            tasks.erase(iterator);
            active_count.fetch_add(1, std::memory_order_acq_rel);
            break;
        }
        return result;
    }

    bool scheduler::has_runnable_task_no_lock(thread_id current_thread) const
    {
        for (const task& item : tasks)
        {
            if (item.valid() && (item.id == any_thread || item.id == current_thread))
            {
                return true;
            }
        }
        return false;
    }

    void scheduler::tickle()
    {
        cv.notify_all();
    }

    void scheduler::idle()
    {
        const thread_id mine = utility::thread_info::get_local().id();
        while (!can_stop())
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this, mine] {
                return has_runnable_task_no_lock(mine) || can_stop_no_lock();
            });
            lock.unlock();
            if (!can_stop())
            {
                fiber::yield_to_hold();
            }
        }
    }

    void scheduler::run()
    {
        scheduler* const previous_scheduler = current_scheduler;
        fiber* const previous_scheduler_fiber = current_scheduler_fiber;

        set_this();
        on_thread_enter();
        current_scheduler_fiber = fiber::current();
        const thread_id mine = utility::thread_info::get_local().id();
        if (mine != root_thread_id)
        {
            std::lock_guard lock(mutex);
            thread_ids.push_back(mine);
            ++registered_workers;
            cv.notify_all();
        }

        auto idle_fiber = std::make_shared<fiber>([this] { idle(); });
        fiber::shared callback_fiber;

        for (;;)
        {
            fetch_result fetched = fetch_task(mine);
            if (fetched.tickle_others)
            {
                tickle();
            }
            if (!fetched.fetched.valid())
            {
                if (can_stop())
                {
                    break;
                }
                if (fetched.retry_pending)
                {
                    std::this_thread::yield();
                    continue;
                }

                idle_count.fetch_add(1, std::memory_order_acq_rel);
                fiber::state idle_state = idle_fiber->get_state();
                if (idle_state == fiber::state::waiting)
                {
                    (void)idle_fiber->wake();
                    idle_state = idle_fiber->get_state();
                }
                if (idle_state == fiber::state::init || idle_state == fiber::state::ready)
                {
                    idle_fiber->resume();
                }
                idle_count.fetch_sub(1, std::memory_order_acq_rel);

                if (idle_fiber->get_state() == fiber::state::error)
                {
                    report_fiber_failure(idle_fiber);
                    break;
                }
                if (idle_fiber->get_state() == fiber::state::terminated)
                {
                    break;
                }
                continue;
            }

            fiber::shared running;
            const bool callback_task = fetched.fetched.as_callback() != nullptr;
            if (fiber::shared* scheduled = fetched.fetched.as_fiber())
            {
                running = std::move(*scheduled);
            }
            else if (callback* entry = fetched.fetched.as_callback())
            {
                if (callback_fiber && (callback_fiber->get_state() == fiber::state::terminated ||
                                       callback_fiber->get_state() == fiber::state::error))
                {
                    callback_fiber->reset(std::move(*entry));
                }
                else
                {
                    callback_fiber = std::make_shared<fiber>(std::move(*entry));
                }
                running = callback_fiber;
            }

            const fiber::state suspend_state = running->resume();

            if (suspend_state == fiber::state::ready)
            {
                add_task(running);
                if (callback_task)
                {
                    callback_fiber.reset();
                }
            }
            else if (suspend_state == fiber::state::waiting)
            {
                if (callback_task)
                {
                    callback_fiber.reset();
                }
            }
            else if (suspend_state == fiber::state::error)
            {
                report_fiber_failure(running);
            }
            task_finished();
        }

        current_scheduler = previous_scheduler;
        current_scheduler_fiber = previous_scheduler_fiber;
    }

    void scheduler::task_finished() noexcept
    {
        {
            std::lock_guard lock(mutex);
            active_count.fetch_sub(1, std::memory_order_acq_rel);
        }
        cv.notify_all();
    }

    bool scheduler::can_stop()
    {
        std::lock_guard lock(mutex);
        return can_stop_no_lock();
    }

    bool scheduler::can_stop_no_lock() const noexcept
    {
        return auto_stop.load(std::memory_order_acquire)
            && stopping.load(std::memory_order_acquire)
            && tasks.empty()
            && active_count.load(std::memory_order_acquire) == 0;
    }

    void scheduler::stop()
    {
        if (get_current() == this && (!use_caller_thread || utility::thread_info::get_local().id() != root_thread_id))
        {
            utility::log_fatal(std::source_location::current(),
                               "scheduler::stop cannot join its current worker thread");
        }
        if (use_caller_thread && utility::thread_info::get_local().id() != root_thread_id)
        {
            utility::log_fatal(std::source_location::current(),
                               "a caller scheduler must be stopped by its construction thread");
        }

        std::size_t wake_count = 0;
        {
            std::lock_guard lock(mutex);
            auto_stop.store(true, std::memory_order_release);
            stopping.store(true, std::memory_order_release);
            if (!started && workers.empty() && tasks.empty())
            {
                return;
            }
            wake_count = thread_count + (root_fiber ? 1U : 0U);
        }
        cv.notify_all();
        for (std::size_t index = 0; index < wake_count; ++index)
        {
            tickle();
        }

        if (root_fiber && !can_stop())
        {
            fiber::state state = root_fiber->get_state();
            if (state == fiber::state::waiting)
            {
                (void)root_fiber->wake();
                state = root_fiber->get_state();
            }
            if (state == fiber::state::init || state == fiber::state::ready)
            {
                root_fiber->resume();
            }
            if (root_fiber->get_state() == fiber::state::error)
            {
                report_fiber_failure(root_fiber);
            }
        }

        std::vector<std::thread> joined;
        {
            std::lock_guard lock(mutex);
            joined.swap(workers);
        }
        for (std::thread& worker : joined)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        {
            std::lock_guard lock(mutex);
            started = false;
            registered_workers = 0;
            if (use_caller_thread)
            {
                thread_ids.erase(std::remove_if(thread_ids.begin(), thread_ids.end(), [this](thread_id id) {
                                     return id != root_thread_id;
                                 }),
                                 thread_ids.end());
            }
            else
            {
                thread_ids.clear();
            }
        }
    }

    void scheduler::switch_to(thread_id id)
    {
        scheduler* const source = get_current();
        if (source == this && (id == any_thread || id == utility::thread_info::get_local().id()))
        {
            return;
        }

        fiber::shared current = fiber::shared_current();
        if (!current)
        {
            utility::log_fatal(std::source_location::current(),
                               "scheduler::switch_to must be called inside a fiber");
        }
        add_task(current, id);
        fiber::yield_to_hold();
    }

    void scheduler::report_fiber_failure(const fiber::shared& failed) const noexcept
    {
        const std::shared_ptr<std::string> message = failed->get_error();
        if (message != nullptr && !message->empty())
        {
            utility::log_error(std::source_location::current(), "scheduler={} fiber={} failed: {}",
                               scheduler_name, failed->id(), *message);
        }
    }

    std::ostream& scheduler::dump(std::ostream& output) const
    {
        std::lock_guard lock(mutex);
        output << "[scheduler name=" << scheduler_name
               << " workers=" << thread_count
               << " active=" << active_count.load(std::memory_order_acquire)
               << " idle=" << idle_count.load(std::memory_order_acquire)
               << " stopping=" << stopping.load(std::memory_order_acquire)
               << "]\n    ";
        for (std::size_t index = 0; index < thread_ids.size(); ++index)
        {
            if (index != 0)
            {
                output << ", ";
            }
            output << thread_ids[index];
        }
        return output;
    }

    scheduler_switcher::scheduler_switcher(scheduler* target)
      : caller(scheduler::get_current())
    {
        if (target != nullptr)
        {
            target->switch_to();
        }
    }

    scheduler_switcher::~scheduler_switcher() noexcept
    {
        if (caller != nullptr)
        {
            caller->switch_to();
        }
    }
}