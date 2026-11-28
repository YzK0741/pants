//
// Created by 23530 on 2026/11/28.
//
module;

#include <chrono>
#include <string_view>

export module utility.clock;

export namespace utility
{
    [[nodiscard]] std::string_view seconds_text() noexcept;
    [[nodiscard]] std::string_view seconds_text(std::chrono::system_clock::time_point when) noexcept;
}
