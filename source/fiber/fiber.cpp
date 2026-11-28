//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

#include "fcontext_asm.h"

module fiber;

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
        if (stack_base != nullptr)
        {
            (void)::munmap(stack_base, stack_bytes);
            stack_base = nullptr;
            stack_bytes = 0;
        }
        live_fiber_count.fetch_sub(1, std::memory_order_relaxed);
    }

    bool fiber::initialize_context(callback func)
    {
        if (!func)
        {
            utility::log_fatal(std::source_location::current(), "fiber: callback must not be empty");
            return false;
        }

        const long page_value = ::sysconf(_SC_PAGESIZE);
        const std::size_t page = page_value > 0 ? static_cast<std::size_t>(page_value) : 4096U;
        if (options.stack_size < 16 * 1024)
        {
            utility::log_fatal(std::source_location::current(), "fiber: stack size is too small");
            return false;
        }

        const std::size_t guard_bytes = options.guard_page ? page : 0;
        const std::size_t usable = ((options.stack_size + page - 1) / page) * page;
        const std::size_t total = usable + guard_bytes;

        void* const base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (base == MAP_FAILED)
        {
            utility::log_fatal(std::source_location::current(), "fiber: failed to map stack");
            return false;
        }
        if (guard_bytes != 0)
        {
            (void)::mprotect(base, guard_bytes, PROT_NONE);
        }

        void* const stack_top = static_cast<char*>(base) + total;
        void* const new_context = make_fcontext(stack_top, usable, &fiber::entry);
        if (new_context == nullptr)
        {
            (void)::munmap(base, total);
            utility::log_fatal(std::source_location::current(), "fiber: failed to create context");
            return false;
        }

        if (stack_base != nullptr)
        {
            (void)::munmap(stack_base, stack_bytes);
        }
        stack_base = base;
        stack_bytes = total;

        {
            std::lock_guard lock(error_mutex);
            error = nullptr;
        }

        this->func = std::move(func);
        caller = nullptr;
        context = new_context;
        reason.store(wait_reason::none, std::memory_order_relaxed);
        parked.store(true, std::memory_order_relaxed);
        suspend_status.store(state::init, std::memory_order_relaxed);
        status.store(state::init, std::memory_order_release);
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

        if (!context)
        {
            save_logic_error("fiber has no resumable context");
            suspend_status.store(state::error, std::memory_order_relaxed);
            status.store(state::error, std::memory_order_release);
            return state::error;
        }

        parked.store(false, std::memory_order_relaxed);
        current_fiber_guard guard(this);
        const fcontext_transfer_t transferred = jump_fcontext(context, this);
        context = transferred.fctx;

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
        return options.stack_size;
    }

    bool fiber::guard_page() const noexcept
    {
        return options.guard_page;
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
