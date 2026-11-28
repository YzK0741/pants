//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <queue>
#include <source_location>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>
#include <cstdlib>

module fiber.stack;

import log;

namespace pants
{
    namespace
    {
        constexpr std::size_t canary_bytes = 64;
        constexpr std::uint64_t canary_value = 0x9E3779B97F4A7C15ULL;
        constexpr std::size_t growth_segment_bytes = 4U * 1024U * 1024U;

        [[nodiscard]] std::size_t page_size() noexcept
        {
            static const std::size_t value = [] {
                const long probe = ::sysconf(_SC_PAGESIZE);
                return probe > 0 ? static_cast<std::size_t>(probe) : 4096U;
            }();
            return value;
        }

        [[nodiscard]] std::size_t round_up(std::size_t value, std::size_t alignment) noexcept
        {
            return (value + alignment - 1) / alignment * alignment;
        }
    }

    fiber_stack_resource& fiber_stack_resource::instance()
    {
        return instance(config{});
    }

    fiber_stack_resource& fiber_stack_resource::instance(const config& options)
    {
        static fiber_stack_resource resource(options);
        return resource;
    }

    fiber_stack_resource::fiber_stack_resource(const config& options)
      : segment_bytes(round_up(std::max<std::size_t>(options.segment_bytes, page_size()), page_size()))
      , min_stack_bytes(round_up(std::max<std::size_t>(options.min_stack_bytes, 4U * 1024U), page_size()))
      , guard_bytes(round_up(options.guard_bytes == 0 ? page_size() : options.guard_bytes, page_size()))
      , overflow_mode(options.mode)
    {
    }

    fiber_stack_resource::~fiber_stack_resource()
    {
        std::lock_guard lock(mutex);
        for (auto& [usable, pool] : classes)
        {
            (void)usable;
            for (const segment& current : pool.segments)
            {
                (void)::munmap(current.base, current.size);
            }
        }
        classes.clear();
    }

    std::size_t fiber_stack_resource::normalize_stack(std::size_t stack_bytes) const noexcept
    {
        return round_up(std::max<std::size_t>(stack_bytes, min_stack_bytes), page_size());
    }

    std::size_t fiber_stack_resource::overflow_bytes() const noexcept
    {
        if (overflow_mode == stack_overflow_mode::guard_page)
        {
            return guard_bytes;
        }
        if (overflow_mode == stack_overflow_mode::canary)
        {
            return canary_bytes;
        }
        return 0;
    }

    std::size_t fiber_stack_resource::usable_stack_size(std::size_t stack_bytes) const noexcept
    {
        return normalize_stack(stack_bytes);
    }

    std::byte* fiber_stack_resource::pop_slot(stack_class& pool) noexcept
    {
        if (!pool.free_slots.empty())
        {
            std::byte* const slot = pool.free_slots.back();
            pool.free_slots.pop_back();
            return slot;
        }

        for (segment& current : pool.segments)
        {
            if (current.next_index < current.slot_count)
            {
                std::byte* const slot = current.base + current.next_index * pool.slot_bytes;
                ++current.next_index;
                return slot;
            }
        }
        return nullptr;
    }

    void fiber_stack_resource::push_slot(stack_class& pool, std::byte* slot) noexcept
    {
        pool.free_slots.push_back(slot);
    }

    bool fiber_stack_resource::grow(stack_class& pool, std::size_t request_bytes)
    {
        const std::size_t size = round_up(std::max(request_bytes, growth_segment_bytes), pool.slot_bytes);
        void* const base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (base == MAP_FAILED)
        {
            return false;
        }
        pool.segments.push_back(segment{static_cast<std::byte*>(base), size, size / pool.slot_bytes, 0});
        return true;
    }

    fiber_stack_resource::stack_class& fiber_stack_resource::get_class(std::size_t usable_bytes)
    {
        auto position = classes.find(usable_bytes);
        if (position != classes.end())
        {
            return position->second;
        }

        stack_class pool;
        pool.usable_bytes = usable_bytes;
        pool.guard_bytes = overflow_bytes();
        pool.slot_bytes = round_up(usable_bytes + pool.guard_bytes, page_size());
        auto [inserted, _] = classes.emplace(usable_bytes, std::move(pool));
        (void)_;
        return inserted->second;
    }

    void* fiber_stack_resource::acquire_stack(std::size_t stack_bytes)
    {
        const std::size_t usable = normalize_stack(stack_bytes);

        std::lock_guard lock(mutex);
        stack_class& pool = get_class(usable);
        std::byte* slot = pop_slot(pool);
        if (slot == nullptr && !grow(pool, segment_bytes))
        {
            utility::log_fatal(std::source_location::current(), "fiber_stack_resource: cannot reserve stack");
        }
        if (slot == nullptr)
        {
            slot = pop_slot(pool);
        }
        if (slot == nullptr)
        {
            return nullptr;
        }

        ++pool.used;
        if (overflow_mode == stack_overflow_mode::canary)
        {
            std::memcpy(slot, &canary_value, sizeof(canary_value));
            return slot + canary_bytes;
        }
        if (overflow_mode == stack_overflow_mode::guard_page)
        {
            (void)::mprotect(slot, pool.guard_bytes, PROT_NONE);
            return slot + pool.guard_bytes;
        }
        return slot;
    }

    void fiber_stack_resource::release_stack(void* stack_pointer, std::size_t stack_bytes) noexcept
    {
        if (stack_pointer == nullptr)
        {
            return;
        }

        const std::size_t usable = normalize_stack(stack_bytes);
        auto* const pointer = static_cast<std::byte*>(stack_pointer);
        std::byte* slot;

        if (overflow_mode == stack_overflow_mode::canary)
        {
            slot = pointer - canary_bytes;
        }
        else if (overflow_mode == stack_overflow_mode::guard_page)
        {
            slot = pointer - guard_bytes;
        }
        else
        {
            slot = pointer;
        }

        std::lock_guard lock(mutex);
        const auto position = classes.find(usable);
        if (position == classes.end())
        {
            utility::log_error(std::source_location::current(),
                               "fiber_stack_resource: released stack of unknown size class {}", usable);
            return;
        }
        push_slot(position->second, slot);
        --position->second.used;
    }

    bool fiber_stack_resource::validate_stack(void* stack_pointer, std::size_t /*stack_bytes*/) const noexcept
    {
        if (stack_pointer == nullptr || overflow_mode != stack_overflow_mode::canary)
        {
            return true;
        }
        std::uint64_t value = 0;
        std::memcpy(&value, static_cast<std::byte*>(stack_pointer) - canary_bytes, sizeof(value));
        return value == canary_value;
    }

    const fiber_stack_resource::stack_class* fiber_stack_resource::find_class(const void* pointer) const noexcept
    {
        const auto* address = static_cast<const std::byte*>(pointer);
        for (const auto& [usable, pool] : classes)
        {
            (void)usable;
            for (const segment& current : pool.segments)
            {
                if (address >= current.base && address < current.base + current.size)
                {
                    return &pool;
                }
            }
        }
        return nullptr;
    }

    void* fiber_stack_resource::do_allocate(std::size_t bytes, std::size_t alignment)
    {
        if (alignment <= page_size())
        {
            if (void* const stack = acquire_stack(bytes))
            {
                return stack;
            }
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void fiber_stack_resource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment)
    {
        bool inside_stack;
        {
            std::lock_guard lock(mutex);
            inside_stack = find_class(pointer) != nullptr;
        }
        if (inside_stack)
        {
            release_stack(pointer, bytes);
            return;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool fiber_stack_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
    {
        return this == &other;
    }

    stack_pool::stack_pool(std::size_t slot_count, std::size_t slot_bytes)
    {
        this->slot_bytes = round_up(std::max(slot_bytes, static_cast<std::size_t>(16U * 1024U)), page_size());
        const std::size_t count = std::max<std::size_t>(slot_count, 1U);
        blocks.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            void* const base = ::mmap(nullptr, this->slot_bytes, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (base == MAP_FAILED)
            {
                utility::log_fatal(std::source_location::current(), "stack_pool: cannot map shared stack");
            }
            blocks.push_back(static_cast<std::byte*>(base));
        }
        free_slots.reserve(blocks.size());
        for (std::size_t index = blocks.size(); index > 0; --index)
        {
            free_slots.push_back(index - 1);
        }
    }

    stack_pool::~stack_pool()
    {
        std::lock_guard lock(mutex);
        for (std::byte* block : blocks)
        {
            (void)::munmap(block, slot_bytes);
        }
        blocks.clear();
    }

    stack_info stack_pool::acquire()
    {
        std::lock_guard lock(mutex);
        stack_info info;
        if (free_slots.empty())
        {
            return info;
        }
        const std::size_t index = free_slots.back();
        free_slots.pop_back();
        info.base = blocks[index];
        info.size = slot_bytes;
        info.top = blocks[index] + slot_bytes;
        info.mode = stack_mode::shared;
        return info;
    }

    void stack_pool::release(const stack_info& info)
    {
        if (info.base == nullptr)
        {
            return;
        }
        std::lock_guard lock(mutex);
        for (std::size_t index = 0; index < blocks.size(); ++index)
        {
            if (blocks[index] == info.base)
            {
                free_slots.push_back(index);
                return;
            }
        }
    }

    std::size_t stack_pool::slot_size() const noexcept
    {
        return slot_bytes;
    }

    std::size_t stack_pool::free_count() const noexcept
    {
        std::lock_guard lock(mutex);
        return free_slots.size();
    }

    void stack_strategy::set_running(void* owner, bool running) noexcept
    {
        (void)owner;
        (void)running;
    }

    bool stack_strategy::validate(void* owner) const
    {
        (void)owner;
        return true;
    }

    void stack_strategy::set_evict_callbacks(evict_callback switch_stack, guard_callback begin_hook,
                                             evict_callback end_hook) noexcept
    {
        evict.store(switch_stack, std::memory_order_release);
        begin_exchange.store(begin_hook, std::memory_order_release);
        end_exchange.store(end_hook, std::memory_order_release);
    }


    stack_mode dedicated_strategy::mode() const noexcept
    {
        return stack_mode::dedicated;
    }

    bool dedicated_strategy::save_on_suspend() const noexcept
    {
        return false;
    }

    stack_info dedicated_strategy::acquire(void* owner, std::size_t stack_bytes)
    {
        std::lock_guard lock(mutex);
        if (const auto position = held.find(owner); position != held.end())
        {
            return position->second;
        }

        stack_info info;
        void* const stack = fiber_stack_resource::instance().acquire_stack(stack_bytes);
        if (stack == nullptr)
        {
            return info;
        }
        info.base = stack;
        info.size = fiber_stack_resource::instance().usable_stack_size(stack_bytes);
        info.top = static_cast<std::byte*>(stack) + info.size;
        info.mode = stack_mode::dedicated;
        held[owner] = info;
        return info;
    }

    void dedicated_strategy::release(void* owner)
    {
        std::lock_guard lock(mutex);
        const auto position = held.find(owner);
        if (position == held.end())
        {
            return;
        }
        fiber_stack_resource::instance().release_stack(position->second.base, position->second.size);
        held.erase(position);
    }

    bool dedicated_strategy::validate(void* owner) const
    {
        std::lock_guard lock(mutex);
        const auto position = held.find(owner);
        if (position == held.end())
        {
            return true;
        }
        return fiber_stack_resource::instance().validate_stack(position->second.base, position->second.size);
    }

    shared_eager_strategy::shared_eager_strategy(std::size_t slot_count, std::size_t slot_bytes)
      : pool(slot_count, slot_bytes)
    {
    }

    stack_mode shared_eager_strategy::mode() const noexcept
    {
        return stack_mode::shared;
    }

    bool shared_eager_strategy::save_on_suspend() const noexcept
    {
        return true;
    }

    stack_info shared_eager_strategy::acquire(void* owner, std::size_t /*stack_bytes*/)
    {
        stack_info info = pool.acquire();
        if (info.base == nullptr)
        {
            return info;
        }
        std::lock_guard lock(mutex);
        held[owner] = info;
        return info;
    }

    void shared_eager_strategy::release(void* owner)
    {
        stack_info info;
        {
            std::lock_guard lock(mutex);
            const auto position = held.find(owner);
            if (position == held.end())
            {
                return;
            }
            info = position->second;
            held.erase(position);
        }
        pool.release(info);
    }

    std::size_t shared_eager_strategy::slot_size() const noexcept
    {
        return pool.slot_size();
    }

    shared_lazy_strategy::shared_lazy_strategy(std::size_t slot_count, std::size_t slot_bytes)
    {
        this->slot_bytes = round_up(std::max(slot_bytes, static_cast<std::size_t>(16U * 1024U)), page_size());
        const std::size_t count = std::max<std::size_t>(slot_count, 1U);
        blocks.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            void* const base = ::mmap(nullptr, this->slot_bytes, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (base == MAP_FAILED)
            {
                utility::log_fatal(std::source_location::current(), "shared_lazy_strategy: cannot map shared stack");
            }
            blocks.push_back(static_cast<std::byte*>(base));
        }
        free_slots.reserve(blocks.size());
        for (std::size_t index = blocks.size(); index > 0; --index)
        {
            free_slots.push_back(index - 1);
        }
    }

    stack_mode shared_lazy_strategy::mode() const noexcept
    {
        return stack_mode::shared;
    }

    bool shared_lazy_strategy::save_on_suspend() const noexcept
    {
        return false;
    }

    stack_info shared_lazy_strategy::slot_info_of(std::size_t index) const noexcept
    {
        stack_info info;
        info.base = blocks[index];
        info.size = slot_bytes;
        info.top = blocks[index] + slot_bytes;
        info.mode = stack_mode::shared;
        return info;
    }

    stack_info shared_lazy_strategy::acquire(void* owner, std::size_t /*stack_bytes*/)
    {
        void* victim = nullptr;
        stack_info info;

        {
            std::lock_guard lock(mutex);
            if (!free_slots.empty())
            {
                const std::size_t index = free_slots.back();
                free_slots.pop_back();
                info = slot_info_of(index);
                holder entry{owner, ++alloc_sequence, index, true};
                holders.push(entry);
                held[owner] = entry;
            }
            else
            {
                std::vector<holder> skipped;
                skipped.reserve(holders.size());
                while (!holders.empty())
                {
                    const holder candidate = holders.top();
                    holders.pop();

                    const auto position = held.find(candidate.owner);
                    if (position == held.end() || position->second.alloc_time != candidate.alloc_time)
                    {
                        continue;
                    }
                    if (position->second.running)
                    {
                        skipped.push_back(position->second);
                        continue;
                    }
                    const guard_callback begin = begin_exchange.load(std::memory_order_acquire);
                    if (begin != nullptr && !begin(candidate.owner))
                    {
                        skipped.push_back(position->second);
                        continue;
                    }

                    held.erase(position);
                    info = slot_info_of(candidate.slot_index);
                    victim = candidate.owner;
                    holder entry{owner, ++alloc_sequence, candidate.slot_index, true};
                    holders.push(entry);
                    held[owner] = entry;
                    break;
                }

                for (const holder& entry_to_restore : skipped)
                {
                    holders.push(entry_to_restore);
                }
            }
        }

        if (victim != nullptr)
        {
            const evict_callback switch_stack = evict.load(std::memory_order_acquire);
            if (switch_stack != nullptr)
            {
                switch_stack(victim);
            }
            const evict_callback finish = end_exchange.load(std::memory_order_acquire);
            if (finish != nullptr)
            {
                finish(victim);
            }
        }
        return info;
    }

    void shared_lazy_strategy::release(void* owner)
    {
        std::lock_guard lock(mutex);
        const auto position = held.find(owner);
        if (position == held.end())
        {
            return;
        }
        free_slots.push_back(position->second.slot_index);
        held.erase(position);
    }

    void shared_lazy_strategy::set_running(void* owner, bool running) noexcept
    {
        std::lock_guard lock(mutex);
        const auto position = held.find(owner);
        if (position == held.end())
        {
            return;
        }
        position->second.running = running;
    }

    std::size_t shared_lazy_strategy::slot_size() const noexcept
    {
        return slot_bytes;
    }

    fiber_stack_manager& fiber_stack_manager::instance()
    {
        return instance(config{});
    }

    fiber_stack_manager& fiber_stack_manager::instance(const config& options)
    {
        static fiber_stack_manager manager(options);
        return manager;
    }

    fiber_stack_manager::fiber_stack_manager(const config& options)
      : eager(options.shared_slot_count, options.shared_slot_size)
      , lazy(options.shared_slot_count, options.shared_slot_size)
      , use_lazy(options.lazy_slots)
    {
    }

    fiber_stack_manager::~fiber_stack_manager() = default;

    stack_strategy& fiber_stack_manager::strategy_for(stack_mode mode) noexcept
    {
        if (mode == stack_mode::dedicated)
        {
            return dedicated;
        }
        return use_lazy.load(std::memory_order_acquire) ? static_cast<stack_strategy&>(lazy)
                                                       : static_cast<stack_strategy&>(eager);
    }

    bool fiber_stack_manager::lazy_slots() const noexcept
    {
        return use_lazy.load(std::memory_order_acquire);
    }

    void fiber_stack_manager::set_lazy_slots(bool enabled) noexcept
    {
        use_lazy.store(enabled, std::memory_order_release);
    }

    void fiber_stack_manager::set_multi_thread(bool enabled) noexcept
    {
        threaded.store(enabled, std::memory_order_release);
    }

    bool fiber_stack_manager::multi_thread() const noexcept
    {
        return threaded.load(std::memory_order_acquire);
    }
}
