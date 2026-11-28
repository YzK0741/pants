//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

module utility.text;

import log;

namespace pants
{
    namespace
    {
        constexpr std::uint32_t max_code_point = 0x10FFFF;
        constexpr std::uint32_t high_surrogate_first = 0xD800;
        constexpr std::uint32_t high_surrogate_last = 0xDBFF;
        constexpr std::uint32_t low_surrogate_first = 0xDC00;
        constexpr std::uint32_t low_surrogate_last = 0xDFFF;

        [[nodiscard]] constexpr bool is_high_surrogate(std::uint32_t value) noexcept
        {
            return value >= high_surrogate_first && value <= high_surrogate_last;
        }

        [[nodiscard]] constexpr bool is_low_surrogate(std::uint32_t value) noexcept
        {
            return value >= low_surrogate_first && value <= low_surrogate_last;
        }

        void append_utf8(std::string& output, std::uint32_t code_point)
        {
            if (code_point <= 0x7F)
            {
                output.push_back(static_cast<char>(code_point));
            }
            else if (code_point <= 0x7FF)
            {
                output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
                output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
            else if (code_point <= 0xFFFF)
            {
                output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
                output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
            else
            {
                output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
                output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
        }

        [[nodiscard]] bool decode_utf8(std::string_view text, std::size_t& offset, std::uint32_t& code_point) noexcept
        {
            if (offset >= text.size())
            {
                return false;
            }

            const auto first = static_cast<unsigned char>(text[offset++]);
            if (first <= 0x7F)
            {
                code_point = first;
                return true;
            }

            std::uint32_t minimum_code_point = 0;
            std::size_t continuation_count = 0;
            if (first >= 0xC2 && first <= 0xDF)
            {
                code_point = first & 0x1FU;
                minimum_code_point = 0x80;
                continuation_count = 1;
            }
            else if (first >= 0xE0 && first <= 0xEF)
            {
                code_point = first & 0x0FU;
                minimum_code_point = 0x800;
                continuation_count = 2;
            }
            else if (first >= 0xF0 && first <= 0xF4)
            {
                code_point = first & 0x07U;
                minimum_code_point = 0x10000;
                continuation_count = 3;
            }
            else
            {
                return false;
            }

            if (text.size() - offset < continuation_count)
            {
                return false;
            }
            for (std::size_t index = 0; index < continuation_count; ++index)
            {
                const auto continuation = static_cast<unsigned char>(text[offset++]);
                if ((continuation & 0xC0U) != 0x80U)
                {
                    return false;
                }
                code_point = (code_point << 6U) | (continuation & 0x3FU);
            }

            return code_point >= minimum_code_point && code_point <= max_code_point
                && !is_high_surrogate(code_point) && !is_low_surrogate(code_point);
        }

    }

    std::string_view trim(std::string_view text, std::string_view delimiters) noexcept
    {
        const std::size_t begin = text.find_first_not_of(delimiters);
        if (begin == std::string_view::npos)
        {
            return {};
        }
        const std::size_t end = text.find_last_not_of(delimiters);
        return text.substr(begin, end - begin + 1);
    }


    std::string_view trim_left(std::string_view text, std::string_view delimiters) noexcept
    {
        const std::size_t begin = text.find_first_not_of(delimiters);
        if (begin == std::string_view::npos)
        {
            return {};
        }
        return text.substr(begin);
    }


    std::string_view trim_right(std::string_view text, std::string_view delimiters) noexcept
    {
        const std::size_t end = text.find_last_not_of(delimiters);
        if (end == std::string_view::npos)
        {
            return {};
        }
        return text.substr(0, end + 1);
    }

    std::string wstring_to_string(std::wstring_view text) noexcept
    {
        std::string result;
        result.reserve(text.size());

        for (std::size_t index = 0; index < text.size(); ++index)
        {
            auto code_point = static_cast<std::uint32_t>(text[index]);
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (is_high_surrogate(code_point))
                {
                    ++index;
                    if (index >= text.size())
                    {
                        utility::log_error(std::source_location::current(), "utils: truncated UTF-16 surrogate pair");
                        return {};
                    }
                    const auto low_surrogate = static_cast<std::uint32_t>(text[index]);
                    if (!is_low_surrogate(low_surrogate))
                    {
                        utility::log_error(std::source_location::current(), "utils: invalid UTF-16 surrogate pair");
                        return {};
                    }
                    code_point = 0x10000U + ((code_point - high_surrogate_first) << 10U)
                        + (low_surrogate - low_surrogate_first);
                }
                else if (is_low_surrogate(code_point))
                {
                    utility::log_error(std::source_location::current(), "utils: isolated UTF-16 low surrogate");
                    return {};
                }
            }
            else if (code_point > max_code_point || is_high_surrogate(code_point) || is_low_surrogate(code_point))
            {
                utility::log_error(std::source_location::current(), "utils: invalid UTF-32 code point");
                return {};
            }

            append_utf8(result, code_point);
        }
        return result;
    }

    std::wstring string_to_wstring(std::string_view text) noexcept
    {
        std::wstring result;
        result.reserve(text.size());

        std::size_t offset = 0;
        while (offset < text.size())
        {
            std::uint32_t code_point = 0;
            if (!decode_utf8(text, offset, code_point))
            {
                utility::log_error(std::source_location::current(), "utils: invalid UTF-8 sequence");
                return {};
            }

            if constexpr (sizeof(wchar_t) == 2)
            {
                if (code_point <= 0xFFFF)
                {
                    result.push_back(static_cast<wchar_t>(code_point));
                }
                else
                {
                    code_point -= 0x10000U;
                    result.push_back(static_cast<wchar_t>(high_surrogate_first + (code_point >> 10U)));
                    result.push_back(static_cast<wchar_t>(low_surrogate_first + (code_point & 0x3FFU)));
                }
            }
            else
            {
                result.push_back(static_cast<wchar_t>(code_point));
            }
        }
        return result;
    }

}
