//
// Created by 23530 on 2026/11/28.
//
module;

#include <cctype>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <functional>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "toml_setup.h"

module config;

import log;

namespace pants
{
    namespace
    {
        struct config_registry
        {
            std::mutex mutex;
            std::unordered_map<std::string, config_var_base::shared> variables;
        };

        config_registry& registry()
        {
            static auto* instance = new config_registry;
            return *instance;
        }

        bool is_name_character(char character) noexcept
        {
            const auto code = static_cast<unsigned char>(character);
            return std::isalnum(code) != 0 || character == '_' || character == '-' || character == '.';
        }

        void walk_table(const toml::table& table, const std::string& prefix)
        {
            for (const auto& [key, node] : table)
            {
                const std::string path = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
                if (const auto* nested = node.as_table(); nested != nullptr)
                {
                    walk_table(*nested, path);
                    continue;
                }

                const config_var_base::shared variable = config::lookup_base(path);
                if (!variable)
                {
                    utility::log_warning(std::source_location::current(), "config: ignore unknown key '{}'", path);
                    continue;
                }
                if (!variable->set_from_node(node))
                {
                    utility::log_error(std::source_location::current(), "config: failed to apply '{}'", path);
                }
            }
        }
    }

    bool config::is_valid_name(std::string_view name) noexcept
    {
        if (name.empty())
        {
            return false;
        }

        bool previous_is_dot = true;
        for (const char character : name)
        {
            if (character == '.')
            {
                if (previous_is_dot)
                {
                    return false;
                }
                previous_is_dot = true;
                continue;
            }
            if (!is_name_character(character))
            {
                return false;
            }
            previous_is_dot = false;
        }
        return !previous_is_dot;
    }

    std::string config::normalize_name(std::string_view name)
    {
        std::string normalized;
        normalized.reserve(name.size());
        for (const char character : name)
        {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
        return normalized;
    }

    config_var_base::shared config::lookup_base(std::string_view name)
    {
        const std::string key = normalize_name(name);
        std::lock_guard lock(registry().mutex);
        const auto position = registry().variables.find(key);
        return position == registry().variables.end() ? nullptr : position->second;
    }

    config_var_base::shared config::lookup_or_create(std::string_view name, std::string_view type,
                                                     const create_fn& create)
    {
        if (!is_valid_name(name))
        {
            utility::log_error(std::source_location::current(), "config: invalid variable name '{}'", name);
            return nullptr;
        }

        const std::string key = normalize_name(name);
        std::lock_guard lock(registry().mutex);

        if (const auto position = registry().variables.find(key); position != registry().variables.end())
        {
            if (position->second->type_name() != type)
            {
                return nullptr;
            }
            return position->second;
        }

        config_var_base::shared created = create(key);
        if (!created)
        {
            return nullptr;
        }
        registry().variables.emplace(key, created);
        return created;
    }

    void config::load_string(std::string_view text)
    {
        toml::parse_result result = toml::parse(text);
        if (!result)
        {
            utility::log_error(std::source_location::current(), "config: {}",
                               result.error().description());
            return;
        }
        walk_table(result.table(), {});
    }

    void config::load_file(std::string_view path)
    {
        std::ifstream input{std::string(path)};
        if (!input.is_open())
        {
            utility::log_error(std::source_location::current(), "config: cannot open '{}'", path);
            return;
        }

        std::string text;
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        load_string(text);
    }

    void config::visit(const visit_fn& callback)
    {
        if (!callback)
        {
            return;
        }
        std::lock_guard lock(registry().mutex);
        for (const auto& [name, variable] : registry().variables)
        {
            (void)name;
            callback(variable);
        }
    }

    void config::clear_all_listeners()
    {
        std::lock_guard lock(registry().mutex);
        for (const auto& [name, variable] : registry().variables)
        {
            (void)name;
            variable->clear_listeners();
        }
    }

    std::size_t config::size()
    {
        std::lock_guard lock(registry().mutex);
        return registry().variables.size();
    }

    config_var_base::config_var_base(std::string name, std::string description)
      : var_name(std::move(name))
      , var_description(std::move(description))
    {
    }

    const std::string& config_var_base::name() const noexcept
    {
        return var_name;
    }

    const std::string& config_var_base::description() const noexcept
    {
        return var_description;
    }
}
