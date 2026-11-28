//
// Created by 23530 on 2026/11/28.
//
module;

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <execinfo.h>

module utility.diagnostics;

namespace pants
{
    std::chrono::steady_clock::duration elapsed_time() noexcept
    {
        static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        return std::chrono::steady_clock::now() - start;
    }

    std::string backtrace(int size, int skip, const std::string& prefix)
    {
        if (size <= 0)
        {
            return {};
        }

        std::vector<void*> frames(static_cast<std::size_t>(size) + static_cast<std::size_t>(skip), nullptr);
        const int captured = ::backtrace(frames.data(), static_cast<int>(frames.size()));
        if (captured <= 0)
        {
            return {};
        }

        std::string result;
        char** symbols = ::backtrace_symbols(frames.data(), captured);
        if (symbols == nullptr)
        {
            return result;
        }

        for (int index = skip; index < captured; ++index)
        {
            result += prefix;
            result += symbols[index];
            result.push_back('\n');
        }
        std::free(symbols);
        return result;
    }
}
