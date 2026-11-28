//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

#include "fcontext_asm.h"

module fiber;

import fiber.stack;
import log;

namespace pants
{

    namespace
    {
        std::atomic<std::uint64_t> next_fiber_id{0};
        std::atomic<std::uint64_t> live_fiber_count{0};
        thread_local fiber* current_fiber = nullptr;

        class current_fiber_guard
        {
            public:
            explicit current_fiber_guard(fiber* value) noexcept
              : previous(current_fiber)
            {
                current_fiber = value;
            }

            ~current_fiber_guard() noexcept
            {
                current_fiber = previous;
            }

            current_fiber_guard(const current_fiber_guard&) = delete;
            current_fiber_guard& operator=(const current_fiber_guard&) = delete;

            private:
            fiber* previous;
        };
    }

    fiber::fiber(callback func) noexcept
      : fiber(std::move(func), option{})
    {
    }

    fiber::fiber(callback func, option const& opt) noexcept
      : ident(next_fiber_id.fetch_add(1, std::memory_order_relaxed) + 1)
      , options(opt)
    {
        (void)initialize_context(std::move(func));
        live_fiber_count.fetch_add(1, std::memory_order_relaxed);
    }

    fiber::~fiber() noexcept
    {
        if (current_fiber == this)
        {
            std::terminate();
        }

        bool expected_exchange = false;
        while (!stack_exchanging.compare_exchange_weak(expected_exchange, true, std::memory_order_acq_rel))
        {
            expected_exchange = false;
            std::this_thread::yield();
        }

        if (strategy != nullptr && !strategy->validate(this))
        {
            utility::log_error(std::source_location::current(), "fiber: stack overflow detected on destroyed fiber {}", ident);
        }
        release_stack();
        saved_stack.clear();
        saved_bytes = 0;
        live_fiber_count.fetch_sub(1, std::memory_order_relaxed);
    }

    bool fiber::initialize_context(callback func)
    {
        if (!func)
        {
            utility::log_fatal(std::source_location::current(), "fiber: callback must not be empty");
            return false;
        }

        bool expected_exchange = false;
        while (!stack_exchanging.compare_exchange_weak(expected_exchange, true, std::memory_order_acq_rel))
        {
            expected_exchange = false;
            std::this_thread::yield();
        }

        release_stack();
        fiber_stack_manager& stack_manager = fiber_stack_manager::instance();
        strategy = &stack_manager.strategy_for(options.stack_mode);
        strategy->set_evict_callbacks(&fiber::evict_trampoline, &fiber::begin_exchange_trampoline,
                                      &fiber::end_exchange_trampoline);
        context = nullptr;
        saved_stack.clear();
        saved_bytes = 0;

        {
            std::lock_guard lock(error_mutex);
            error = nullptr;
        }

        this->func = std::move(func);
        caller = nullptr;
        reason.store(wait_reason::none, std::memory_order_relaxed);
        parked.store(true, std::memory_order_relaxed);
        suspend_status.store(state::init, std::memory_order_relaxed);
        status.store(state::init, std::memory_order_release);
        stack_exchanging.store(false, std::memory_order_release);
        return true;
    }
    fiber::state fiber::resume()
    {
        state expected = status.load(std::memory_order_acquire);
        if (expected == state::init)
        {
            if (!status.compare_exchange_strong(expected, state::ready, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                save_logic_error("fiber state changed concurrently while becoming ready");
                return status.load(std::memory_order_acquire);
            }
            expected = state::ready;
        }

        if (expected != state::ready)
        {
            save_logic_error("only init or ready fiber can be resumed");
            return status.load(std::memory_order_acquire);
        }

        if (!status.compare_exchange_strong(expected, state::executing, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            save_logic_error("fiber was resumed concurrently");
            return status.load(std::memory_order_acquire);
        }

        while (!parked.load(std::memory_order_acquire))
        {
            parked.wait(false, std::memory_order_acquire);
        }

        bool expected_exchange = false;
        while (!stack_exchanging.compare_exchange_weak(expected_exchange, true, std::memory_order_acq_rel))
        {
            expected_exchange = false;
            std::this_thread::yield();
        }

        if (!prepare_stack())
        {
            save_logic_error("fiber has no resumable stack");
            stack_exchanging.store(false, std::memory_order_release);
            suspend_status.store(state::error, std::memory_order_relaxed);
            status.store(state::error, std::memory_order_release);
            return state::error;
        }

        parked.store(false, std::memory_order_relaxed);
        current_fiber_guard guard(this);

        if (strategy != nullptr)
        {
            strategy->set_running(this, true);
        }

        const fcontext_transfer_t transferred = jump_fcontext(context, this);
        context = transferred.fctx;

        if (strategy != nullptr)
        {
            strategy->set_running(this, false);
        }

        if (status.load(std::memory_order_acquire) == state::terminated)
        {
            if (strategy != nullptr && !strategy->validate(this))
            {
                utility::log_error(std::source_location::current(), "fiber: stack overflow detected on fiber {}", ident);
            }
            release_stack();
        }
        else if (strategy != nullptr && strategy->save_on_suspend())
        {
            save_stack();
            release_stack();
        }

        stack_exchanging.store(false, std::memory_order_release);

        const state suspended = suspend_status.load(std::memory_order_acquire);
        parked.store(true, std::memory_order_release);
        parked.notify_all();
        return suspended;
    }

    void fiber::yield(state target, wait_reason why)
    {
        reason.store(why, std::memory_order_relaxed);
        state expected = state::executing;
        if (!status.compare_exchange_strong(expected, target, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            reason.store(wait_reason::none, std::memory_order_relaxed);
            save_logic_error("only running fiber can yield");
            return;
        }
        suspend_status.store(target, std::memory_order_release);
        if (!caller)
        {
            save_logic_error("fiber has no caller context to yield to");
            suspend_status.store(state::error, std::memory_order_relaxed);
            status.store(state::error, std::memory_order_release);
            return;
        }

        const fcontext_transfer_t transferred = jump_fcontext(caller, nullptr);
        caller = transferred.fctx;
    }

    void fiber::reset(callback func)
    {
        const state current = status.load(std::memory_order_acquire);
        if (current != state::terminated && current != state::error)
        {
            save_logic_error("fiber can only be reset after terminated or error");
            return;
        }
        if (current_fiber == this)
        {
            save_logic_error("running fiber cannot reset itself");
            return;
        }

        (void)initialize_context(std::move(func));
    }

    bool fiber::wake() noexcept
    {
        if (status.load(std::memory_order_acquire) != state::waiting)
        {
            return false;
        }
        reason.store(wait_reason::none, std::memory_order_relaxed);
        state expected = state::waiting;
        return status.compare_exchange_strong(expected, state::ready,
                                              std::memory_order_release, std::memory_order_relaxed);
    }

    uint64_t fiber::id() const noexcept
    {
        return ident;
    }

    fiber::state fiber::get_state() const noexcept
    {
        return status.load(std::memory_order_acquire);
    }

    fiber::wait_reason fiber::get_wait_reason() const noexcept
    {
        return reason.load(std::memory_order_acquire);
    }

    std::size_t fiber::stack_size() const noexcept
    {
        return stack.size;
    }

    fiber* fiber::current() noexcept
    {
        return current_fiber;
    }

    fiber::shared fiber::shared_current() noexcept
    {
        if (current_fiber == nullptr)
        {
            return nullptr;
        }
        return current_fiber->weak_from_this().lock();
    }

    uint64_t fiber::fiber_id() noexcept
    {
        return current_fiber == nullptr ? 0 : current_fiber->id();
    }

    uint64_t fiber::fiber_count() noexcept
    {
        return live_fiber_count.load(std::memory_order_relaxed);
    }

    void fiber::yield_to_ready() noexcept
    {
        if (current_fiber == nullptr)
        {
            utility::log_fatal(std::source_location::current(), "fiber: yield_to_ready called outside of a fiber");
        }
        current_fiber->yield(state::ready, wait_reason::none);
    }

    void fiber::yield_to_hold() noexcept
    {
        if (current_fiber == nullptr)
        {
            utility::log_fatal(std::source_location::current(), "fiber: yield_to_hold called outside of a fiber");
        }
        current_fiber->yield(state::waiting, wait_reason::external_reason);
    }


    fcontext_transfer_t fiber::entry(fcontext_transfer_t transferred)
    {
        fiber* const self = static_cast<fiber*>(transferred.data);
        self->caller = transferred.fctx;
        self->run();
        jump_fcontext(self->caller, nullptr);
        __builtin_unreachable();
    }

    bool fiber::prepare_stack()
    {
        if (strategy == nullptr)
        {
            strategy = &fiber_stack_manager::instance().strategy_for(options.stack_mode);
            strategy->set_evict_callbacks(&fiber::evict_trampoline, &fiber::begin_exchange_trampoline,
                                          &fiber::end_exchange_trampoline);
        }

        if (stack.base != nullptr)
        {
            return true;
        }

        const stack_info acquired = strategy->acquire(this, options.stack_size);
        if (acquired.base == nullptr)
        {
            return false;
        }
        stack = acquired;

        if (saved_bytes != 0)
        {
            auto* const destination = static_cast<std::byte*>(stack.top) - saved_bytes;
            std::memcpy(destination, saved_stack.data(), saved_bytes);
            context = destination;
            return true;
        }

        context = make_fcontext(stack.top, stack.size, &fiber::entry);
        if (context == nullptr)
        {
            strategy->release(this);
            stack = {};
            return false;
        }
        return true;
    }

    void fiber::release_stack()
    {
        if (strategy != nullptr && stack.base != nullptr)
        {
            strategy->release(this);
        }
        stack = {};
    }

    void fiber::evict_from()
    {
        if (context != nullptr && stack.top != nullptr)
        {
            auto* const sp = static_cast<std::byte*>(context);
            if (sp < stack.top)
            {
                const auto bytes = static_cast<std::size_t>(static_cast<std::byte*>(stack.top) - sp);
                saved_stack.assign(sp, sp + bytes);
                saved_bytes = bytes;
            }
        }
        stack = {};
    }

    void fiber::evict_trampoline(void* owner)
    {
        static_cast<fiber*>(owner)->evict_from();
    }

    bool fiber::begin_exchange_trampoline(void* owner) noexcept
    {
        return !static_cast<fiber*>(owner)->stack_exchanging.exchange(true, std::memory_order_acq_rel);
    }

    void fiber::end_exchange_trampoline(void* owner) noexcept
    {
        static_cast<fiber*>(owner)->stack_exchanging.store(false, std::memory_order_release);
    }

    void fiber::save_stack()
    {
        if (stack.top == nullptr || context == nullptr)
        {
            return;
        }

        auto* const sp = static_cast<std::byte*>(context);
        if (sp >= stack.top)
        {
            return;
        }
        const auto bytes = static_cast<std::size_t>(static_cast<std::byte*>(stack.top) - sp);
        saved_stack.assign(sp, sp + bytes);
        saved_bytes = bytes;
    }

    void fiber::run()
    {
        func();
        func = nullptr;
        reason.store(wait_reason::none, std::memory_order_relaxed);
        suspend_status.store(state::terminated, std::memory_order_relaxed);
        status.store(state::terminated, std::memory_order_release);
    }

    std::shared_ptr<std::string> fiber::get_error() const noexcept
    {
        std::lock_guard lock(error_mutex);
        return error;
    }

    void fiber::save_logic_error(const char* what) noexcept
    {
        auto text = std::make_shared<std::string>(what);
        std::lock_guard lock(error_mutex);
        error = std::move(text);
    }
}
