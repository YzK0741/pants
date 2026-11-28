//
// Created by 23530 on 2026/11/28.
//
module;

#include <cstdint>
#include <string>
#include <string_view>

export module utility.thread;

export namespace pants
{
    [[nodiscard]] std::uint64_t thread_id() noexcept;
    void set_thread_name(std::string name);
    [[nodiscard]] std::string_view thread_name() noexcept;
}
