//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

module utility.thread;

import log;

namespace pants
{
    namespace
    {
        [[nodiscard]] std::string native_thread_name() noexcept
        {
            std::array<char, 16> native_name{};
            if (::pthread_getname_np(::pthread_self(), native_name.data(), native_name.size()) != 0)
            {
                return {};
            }
            return std::string(native_name.data());
        }

        void set_native_thread_name(std::string_view name) noexcept
        {
            std::array<char, 16> native_name{};
            const std::size_t name_size = std::min(name.size(), native_name.size() - 1);
            std::memcpy(native_name.data(), name.data(), name_size);
            (void)::pthread_setname_np(::pthread_self(), native_name.data());
        }

        constexpr std::string_view default_thread_name = "UNKNOWN";
        thread_local std::string local_thread_name = native_thread_name();
    }

    std::uint64_t thread_id() noexcept
    {
        static thread_local const std::uint64_t cached = static_cast<std::uint64_t>(::syscall(SYS_gettid));
        return cached;
    }

    void set_thread_name(std::string name)
    {
        if (name.empty())
        {
            utility::log_error(std::source_location::current(), "thread: empty thread name");
            std::string native = native_thread_name();
            local_thread_name = native.empty() ? std::string(default_thread_name) : std::move(native);
            return;
        }
        local_thread_name = std::move(name);
        set_native_thread_name(local_thread_name);
    }

    std::string_view thread_name() noexcept
    {
        return local_thread_name;
    }
}
