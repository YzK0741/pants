//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

export module fd_manager;

namespace pants
{
    export inline constexpr std::uint64_t invalid_timeout = std::numeric_limits<std::uint64_t>::max();

    export class fd_ctx final
    {
        public:

        using shared = std::shared_ptr<fd_ctx>;

        explicit fd_ctx(int descriptor);

        fd_ctx(const fd_ctx&) = delete;
        fd_ctx& operator=(const fd_ctx&) = delete;

        [[nodiscard]] bool is_init() const noexcept;
        [[nodiscard]] bool is_socket() const noexcept;
        [[nodiscard]] bool is_closed() const noexcept;
        [[nodiscard]] bool is_user_non_block() const noexcept;
        [[nodiscard]] bool is_system_non_block() const noexcept;
        [[nodiscard]] int fd() const noexcept;

        void set_user_non_block(bool value) noexcept;
        void set_timeout(int type, std::uint64_t milliseconds) noexcept;
        [[nodiscard]] std::uint64_t get_timeout(int type) const noexcept;
        void mark_closed() noexcept;

        private:

        void initialize() noexcept;

        std::atomic<std::uint64_t> recv_timeout{invalid_timeout};
        std::atomic<std::uint64_t> send_timeout{invalid_timeout};
        int descriptor{-1};
        bool initialized{false};
        bool socket{false};
        bool system_non_block{false};
        std::atomic<bool> user_non_block{false};
        std::atomic<bool> closed{false};
    };

    export class fd_manager final
    {
        public:

        static fd_manager& instance();

        fd_manager(const fd_manager&) = delete;
        fd_manager& operator=(const fd_manager&) = delete;
        fd_manager(fd_manager&&) = delete;
        fd_manager& operator=(fd_manager&&) = delete;

        [[nodiscard]] fd_ctx::shared get(int fd, bool auto_create = false);
        void del(int fd);
        void close(int fd);
        [[nodiscard]] std::vector<fd_ctx::shared> get_all() const;

        private:

        fd_manager() = default;
        ~fd_manager() = default;

        mutable std::shared_mutex mutex;
        std::unordered_map<int, fd_ctx::shared> contexts;
    };
}
