//
// Created by 23530 on 2026/11/28.
//
module;

#include <concepts>
#include <string>
#include <string_view>

export module utility.text;

export namespace pants
{
    inline constexpr std::string_view ascii_whitespace{" \t\n\r\f\v"};

    [[nodiscard]] std::string_view trim(std::string_view value, std::string_view delimiters = ascii_whitespace) noexcept;
    [[nodiscard]] std::string_view trim_left(std::string_view value, std::string_view delimiters = ascii_whitespace) noexcept;
    [[nodiscard]] std::string_view trim_right(std::string_view value, std::string_view delimiters = ascii_whitespace) noexcept;

    template <typename string_type>
    requires std::convertible_to<const string_type&, std::string_view>
    [[nodiscard]] std::string trim(const string_type& value, std::string_view delimiters = ascii_whitespace)
    {
        return std::string(trim(std::string_view(value), delimiters));
    }

    template <typename string_type>
    requires std::convertible_to<const string_type&, std::string_view>
    [[nodiscard]] std::string trim_left(const string_type& value, std::string_view delimiters = ascii_whitespace)
    {
        return std::string(trim_left(std::string_view(value), delimiters));
    }

    template <typename string_type>
    requires std::convertible_to<const string_type&, std::string_view>
    [[nodiscard]] std::string trim_right(const string_type& value, std::string_view delimiters = ascii_whitespace)
    {
        return std::string(trim_right(std::string_view(value), delimiters));
    }

    [[nodiscard]] std::string wstring_to_string(std::wstring_view value) noexcept;
    [[nodiscard]] std::wstring string_to_wstring(std::string_view value) noexcept;
}
