//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

export module fiber.stack;

export namespace pants
{
    enum class stack_overflow_mode : std::uint8_t
    {
        none,
        canary,
        guard_page,
    };

    enum class stack_mode : std::uint8_t
    {
        dedicated,
        shared,
    };

    struct stack_info
    {
        void* base{nullptr};
        void* top{nullptr};
        std::size_t size{0};
        stack_mode mode{stack_mode::shared};
    };

    class fiber_stack_resource final : public std::pmr::memory_resource
    {
        public:

        struct config
        {
            std::size_t segment_bytes{4U * 1024U * 1024U};
            std::size_t min_stack_bytes{8U * 1024U};
            std::size_t guard_bytes{4096U};
            stack_overflow_mode mode{stack_overflow_mode::canary};
        };

        static fiber_stack_resource& instance();
        static fiber_stack_resource& instance(const config& options);

        explicit fiber_stack_resource(const config& options);
        ~fiber_stack_resource() override;

        fiber_stack_resource(const fiber_stack_resource&) = delete;
        fiber_stack_resource& operator=(const fiber_stack_resource&) = delete;

        [[nodiscard]] void* acquire_stack(std::size_t stack_bytes);
        void release_stack(void* stack_pointer, std::size_t stack_bytes) noexcept;
        [[nodiscard]] bool validate_stack(void* stack_pointer, std::size_t stack_bytes) const noexcept;
        [[nodiscard]] std::size_t usable_stack_size(std::size_t stack_bytes) const noexcept;

        private:

        void* do_allocate(std::size_t bytes, std::size_t alignment) override;
        void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
        [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

        struct segment
        {
            std::byte* base{nullptr};
            std::size_t size{0};
            std::size_t slot_count{0};
            std::size_t next_index{0};
        };

        struct stack_class
        {
            std::size_t slot_bytes{0};
            std::size_t usable_bytes{0};
            std::size_t guard_bytes{0};
            std::vector<segment> segments;
            std::vector<std::byte*> free_slots;
            std::size_t used{0};
        };

        [[nodiscard]] std::size_t normalize_stack(std::size_t stack_bytes) const noexcept;
        [[nodiscard]] std::size_t overflow_bytes() const noexcept;
        [[nodiscard]] stack_class& get_class(std::size_t usable_bytes);
        [[nodiscard]] bool grow(stack_class& pool, std::size_t segment_bytes);
        [[nodiscard]] static std::byte* pop_slot(stack_class& pool) noexcept;
        static void push_slot(stack_class& pool, std::byte* slot) noexcept;
        [[nodiscard]] const stack_class* find_class(const void* pointer) const noexcept;

        std::size_t segment_bytes;
        std::size_t min_stack_bytes;
        std::size_t guard_bytes;
        stack_overflow_mode overflow_mode;
        std::unordered_map<std::size_t, stack_class> classes;
        mutable std::mutex mutex;
    };

    class stack_pool
    {
        public:

        explicit stack_pool(std::size_t slot_count, std::size_t slot_bytes);
        ~stack_pool();

        stack_pool(const stack_pool&) = delete;
        stack_pool& operator=(const stack_pool&) = delete;

        [[nodiscard]] stack_info acquire();
        void release(const stack_info& info);

        [[nodiscard]] std::size_t slot_size() const noexcept;
        [[nodiscard]] std::size_t free_count() const noexcept;

        private:

        mutable std::mutex mutex;
        std::vector<std::byte*> blocks;
        std::vector<std::size_t> free_slots;
        std::size_t slot_bytes{0};
    };

    class stack_strategy
    {
        public:

        using evict_callback = void (*)(void* owner);
        using guard_callback = bool (*)(void* owner);

        virtual ~stack_strategy() = default;

        stack_strategy(const stack_strategy&) = delete;
        stack_strategy& operator=(const stack_strategy&) = delete;

        [[nodiscard]] virtual stack_mode mode() const noexcept = 0;
        [[nodiscard]] virtual bool save_on_suspend() const noexcept = 0;

        [[nodiscard]] virtual stack_info acquire(void* owner, std::size_t stack_bytes) = 0;
        virtual void release(void* owner) = 0;
        virtual void set_running(void* owner, bool running) noexcept;
        [[nodiscard]] virtual bool validate(void* owner) const;

        void set_evict_callbacks(evict_callback switch_stack, guard_callback begin_hook,
                                 evict_callback end_hook) noexcept;

        protected:

        stack_strategy() = default;

        std::atomic<evict_callback> evict{nullptr};
        std::atomic<evict_callback> end_exchange{nullptr};
        std::atomic<guard_callback> begin_exchange{nullptr};
    };

    class dedicated_strategy final : public stack_strategy
    {
        public:

        [[nodiscard]] stack_mode mode() const noexcept override;
        [[nodiscard]] bool save_on_suspend() const noexcept override;
        [[nodiscard]] stack_info acquire(void* owner, std::size_t stack_bytes) override;
        void release(void* owner) override;
        [[nodiscard]] bool validate(void* owner) const override;

        private:

        mutable std::mutex mutex;
        std::unordered_map<void*, stack_info> held;
    };

    class shared_eager_strategy final : public stack_strategy
    {
        public:

        explicit shared_eager_strategy(std::size_t slot_count, std::size_t slot_bytes);

        [[nodiscard]] stack_mode mode() const noexcept override;
        [[nodiscard]] bool save_on_suspend() const noexcept override;
        [[nodiscard]] stack_info acquire(void* owner, std::size_t stack_bytes) override;
        void release(void* owner) override;

        [[nodiscard]] std::size_t slot_size() const noexcept;

        private:

        mutable std::mutex mutex;
        std::unordered_map<void*, stack_info> held;
        stack_pool pool;
    };

    class shared_lazy_strategy final : public stack_strategy
    {
        public:

        explicit shared_lazy_strategy(std::size_t slot_count, std::size_t slot_bytes);

        [[nodiscard]] stack_mode mode() const noexcept override;
        [[nodiscard]] bool save_on_suspend() const noexcept override;
        [[nodiscard]] stack_info acquire(void* owner, std::size_t stack_bytes) override;
        void release(void* owner) override;
        void set_running(void* owner, bool running) noexcept override;

        [[nodiscard]] std::size_t slot_size() const noexcept;

        private:

        struct holder
        {
            void* owner{nullptr};
            std::uint64_t alloc_time{0};
            std::size_t slot_index{0};
            bool running{false};
        };

        struct holder_greater
        {
            bool operator()(const holder& lhs, const holder& rhs) const noexcept
            {
                return lhs.alloc_time > rhs.alloc_time;
            }
        };

        [[nodiscard]] stack_info slot_info_of(std::size_t index) const noexcept;

        mutable std::mutex mutex;
        std::vector<std::byte*> blocks;
        std::vector<std::size_t> free_slots;
        std::priority_queue<holder, std::vector<holder>, holder_greater> holders;
        std::unordered_map<void*, holder> held;
        std::size_t slot_bytes{0};
        std::uint64_t alloc_sequence{0};
    };

    class fiber_stack_manager final
    {
        public:

        struct config
        {
            std::size_t shared_slot_count{64};
            std::size_t shared_slot_size{128U * 1024U};
            stack_mode default_mode{stack_mode::shared};
            bool lazy_slots{true};
        };

        static fiber_stack_manager& instance();
        static fiber_stack_manager& instance(const config& options);

        explicit fiber_stack_manager(const config& options);
        ~fiber_stack_manager();

        fiber_stack_manager(const fiber_stack_manager&) = delete;
        fiber_stack_manager& operator=(const fiber_stack_manager&) = delete;

        [[nodiscard]] stack_strategy& strategy_for(stack_mode mode) noexcept;

        [[nodiscard]] bool lazy_slots() const noexcept;
        void set_lazy_slots(bool enabled) noexcept;

        void set_multi_thread(bool enabled) noexcept;
        [[nodiscard]] bool multi_thread() const noexcept;

        private:

        dedicated_strategy dedicated;
        shared_eager_strategy eager;
        shared_lazy_strategy lazy;
        std::atomic<bool> use_lazy{true};
        std::atomic<bool> threaded{false};
    };
}
