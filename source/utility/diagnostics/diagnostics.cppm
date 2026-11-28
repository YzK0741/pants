//
// Created by 23530 on 2026/11/28.
//
module;

#include <chrono>
#include <cstdint>
#include <string>

export module utility.diagnostics;

export namespace pants
{
    [[nodiscard]] std::chrono::steady_clock::duration elapsed_time() noexcept;
    [[nodiscard]] std::string backtrace(int size = 20, int skip = 1, const std::string& prefix = {});
}
