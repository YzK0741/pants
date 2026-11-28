//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <thread>

module utility.clock;

namespace utility
{
    namespace
    {
        class clock_state final
        {
            public:

            static clock_state& instance()
            {
                static auto* state = new clock_state;
                return *state;
            }

            [[nodiscard]] std::string_view text(std::chrono::system_clock::time_point when) noexcept
            {
                const auto whole = std::chrono::time_point_cast<std::chrono::seconds>(when);
                const auto second = whole.time_since_epoch().count();
                if (second == cached_second.load(std::memory_order_acquire))
                {
                    const std::size_t index = active.load(std::memory_order_acquire);
                    return std::string_view(buffers[index].data(), lengths[index]);
                }

                thread_local std::array<char, 32> scratch{};
                const auto result = std::format_to_n(scratch.data(), scratch.size(), "{:%Y-%m-%d %H:%M:%S}", whole);
                const auto length = std::min(static_cast<std::size_t>(result.size), scratch.size());
                return std::string_view(scratch.data(), length);
            }

            clock_state()
            {
                const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
                fill(0, now);
                cached_second.store(now.time_since_epoch().count(), std::memory_order_release);
                std::thread([this] { run(); }).detach();
            }

            clock_state(const clock_state&) = delete;
            clock_state& operator=(const clock_state&) = delete;

            private:

            void fill(std::size_t index, std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds> whole) noexcept
            {
                const auto result = std::format_to_n(buffers[index].data(), buffers[index].size(),
                                                     "{:%Y-%m-%d %H:%M:%S}", whole);
                lengths[index] = std::min(static_cast<std::size_t>(result.size), buffers[index].size());
            }

            void run() noexcept
            {
                for (;;)
                {
                    const auto now = std::chrono::system_clock::now();
                    const auto whole = std::chrono::time_point_cast<std::chrono::seconds>(now);
                    std::this_thread::sleep_until(whole + std::chrono::seconds(1));

                    const auto tick = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
                    const std::size_t next = 1 - active.load(std::memory_order_relaxed);
                    fill(next, tick);
                    active.store(next, std::memory_order_release);
                    cached_second.store(tick.time_since_epoch().count(), std::memory_order_release);
                }
            }

            std::array<std::array<char, 32>, 2> buffers{};
            std::array<std::size_t, 2> lengths{};
            std::atomic<std::size_t> active{0};
            std::atomic<std::int64_t> cached_second{-1};
        };
    }

    std::string_view seconds_text() noexcept
    {
        return clock_state::instance().text(std::chrono::system_clock::now());
    }

    std::string_view seconds_text(std::chrono::system_clock::time_point when) noexcept
    {
        return clock_state::instance().text(when);
    }
}
