//
// Created by 23530 on 2026/11/27.
//
module;

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>


#include "fcontext_asm.h"

export module fiber;

export import fiber.stack;
import log;

namespace pants
{
    export class fiber : public std::enable_shared_from_this<fiber>
    {
        public:

        using shared = std::shared_ptr<fiber>;
        using callback = std::move_only_function<void()>;

        enum class state : std::uint8_t
        {
            init,
            ready,
            executing,
            waiting,
            terminated,
            error,
        };

        enum class wait_reason : std::uint8_t
        {
            none,
            wait_io,
            wait_timer,
            wait_semaphore,
            external_reason,
        };

        static constexpr std::size_t default_stack_size = 32 * 1024;

        struct option
        {
            std::size_t stack_size = default_stack_size;
            pants::stack_mode stack_mode = pants::stack_mode::shared;
        };

        explicit fiber(callback func) noexcept;
        fiber(callback func, option const& opt) noexcept;
        ~fiber() noexcept;

        fiber(fiber&&) = delete;
        fiber& operator=(fiber&&) = delete;
        fiber(fiber const&) = delete;
        fiber& operator=(fiber const&) = delete;

        state resume();

        void reset(callback func);
        [[nodiscard]] bool wake() noexcept;
        [[nodiscard]] uint64_t id() const noexcept;
        [[nodiscard]] state get_state() const noexcept;
        [[nodiscard]] wait_reason get_wait_reason() const noexcept;
        [[nodiscard]] std::size_t stack_size() const noexcept;
        [[nodiscard]] std::shared_ptr<std::string> get_error() const noexcept;
        [[nodiscard]] static fiber* current() noexcept;
        [[nodiscard]] static shared shared_current() noexcept;
        [[nodiscard]] static uint64_t fiber_id() noexcept;
        [[nodiscard]] static uint64_t fiber_count() noexcept;

        static void yield_to_ready() noexcept;
        static void yield_to_hold() noexcept;
        [[nodiscard]] static constexpr std::string_view to_string(state const value) noexcept
        {
            switch (value)
            {
                case state::init:
                    return "init";
                case state::ready:
                    return "ready";
                case state::executing:
                    return "executing";
                case state::waiting:
                    return "waiting";
                case state::terminated:
                    return "terminated";
                case state::error:
                    return "error";
            }
            return "unknown";
        }

        [[nodiscard]] static constexpr std::string_view to_string(wait_reason const reason) noexcept
        {
            switch (reason)
            {
                case wait_reason::none:
                    return "none";
                case wait_reason::wait_io:
                    return "wait_io";
                case wait_reason::wait_timer:
                    return "wait_timer";
                case wait_reason::wait_semaphore:
                    return "wait_semaphore";
                case wait_reason::external_reason:
                    return "external_reason";
            }
            return "unknown";
        }

        private:

        bool initialize_context(callback func);
        static fcontext_transfer_t entry(fcontext_transfer_t transferred);
        void run();
        [[nodiscard]] bool prepare_stack();
        void release_stack();
        void evict_from();
        static void evict_trampoline(void* owner);
        static bool begin_exchange_trampoline(void* owner) noexcept;
        static void end_exchange_trampoline(void* owner) noexcept;
        void save_stack();
        void yield(state target, wait_reason why);
        void save_logic_error(const char* what) noexcept;

        void* context{nullptr};
        void* caller{nullptr};
        std::atomic<bool> stack_exchanging{false};
        stack_strategy* strategy{nullptr};
        stack_info stack{};
        std::vector<std::byte> saved_stack;
        std::size_t saved_bytes{0};
        callback func;
        std::uint64_t ident{0};
        option options;
        std::atomic<state> status{state::init};
        std::atomic<state> suspend_status{state::init};
        std::atomic<wait_reason> reason{wait_reason::none};
        std::atomic<bool> parked{true};
        mutable std::mutex error_mutex;
        std::shared_ptr<std::string> error;
    };
}