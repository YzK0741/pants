//
// Created by 23530 on 2026/11/28.
//
module;

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "toml_setup.h"

export module config;

import log;

export namespace pants
{
    using listener_id = std::uint64_t;

    template <typename type>
    [[nodiscard]] constexpr std::string_view type_name() noexcept
    {
        std::string_view pretty = __PRETTY_FUNCTION__;
        const std::string_view marker = "type = ";
        const auto begin = pretty.find(marker);
        if (begin == std::string_view::npos)
        {
            return pretty;
        }
        pretty.remove_prefix(begin + marker.size());
        const auto end = pretty.find_first_of(";]");
        if (end == std::string_view::npos)
        {
            return pretty;
        }
        return pretty.substr(0, end);
    }

    class config_var_base
    {
        public:

        using shared = std::shared_ptr<config_var_base>;

        virtual ~config_var_base() = default;

        config_var_base(const config_var_base&) = delete;
        config_var_base& operator=(const config_var_base&) = delete;

        [[nodiscard]] const std::string& name() const noexcept;

        [[nodiscard]] const std::string& description() const noexcept;

        [[nodiscard]] virtual std::string_view type_name() const noexcept = 0;
        virtual bool set_from_node(const toml::node& node) = 0;
        virtual void remove_listener(listener_id id) = 0;
        virtual void clear_listeners() = 0;
        [[nodiscard]] virtual std::size_t listener_count() const = 0;

        protected:

        config_var_base(std::string name, std::string description);

        private:

        std::string var_name;
        std::string var_description;
    };

    template <typename value_type>
    class config_var final : public config_var_base
    {
        public:

        using shared = std::shared_ptr<config_var>;
        using on_change = std::move_only_function<void(const value_type& old_value, const value_type& new_value)>;

        config_var(std::string name, value_type default_value, std::string description = {})
          : config_var_base(std::move(name), std::move(description))
          , value(std::make_shared<const value_type>(std::move(default_value)))
        {
        }

        ~config_var() override
        {
            clear_listeners();
        }

        [[nodiscard]] value_type get() const
        {
            return *value.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::shared_ptr<const value_type> snapshot() const noexcept
        {
            return value.load(std::memory_order_acquire);
        }

        bool set(value_type new_value)
        {
            auto next = std::make_shared<const value_type>(std::move(new_value));
            std::shared_ptr<const value_type> previous;
            {
                std::lock_guard lock(write_mutex);
                previous = value.load(std::memory_order_relaxed);
                if constexpr (std::equality_comparable<value_type>)
                {
                    if (*previous == *next)
                    {
                        return false;
                    }
                }
                value.store(next, std::memory_order_release);
            }
            notify(*previous, *next);
            return true;
        }

        bool set_from_node(const toml::node& node) override
        {
            const std::optional<value_type> parsed = node.value<value_type>();
            if (!parsed)
            {
                utility::log_error(std::source_location::current(), "config: cannot convert value for '{}'", name());
                return false;
            }
            return set(*parsed);
        }

        [[nodiscard]] std::string_view type_name() const noexcept override
        {
            return pants::type_name<value_type>();
        }

        listener_id add_listener(on_change callback)
        {
            if (!callback)
            {
                return 0;
            }
            const listener_id id = next_listener_id.fetch_add(1, std::memory_order_relaxed) + 1;
            auto shared_callback = std::make_shared<on_change>(std::move(callback));
            {
                std::lock_guard lock(listeners_mutex);
                listeners.emplace_back(listener_entry{id, std::move(shared_callback)});
            }
            return id;
        }

        void remove_listener(listener_id id) override
        {
            std::lock_guard lock(listeners_mutex);
            for (auto iterator = listeners.begin(); iterator != listeners.end(); ++iterator)
            {
                if (iterator->id == id)
                {
                    listeners.erase(iterator);
                    return;
                }
            }
        }

        void clear_listeners() override
        {
            std::lock_guard lock(listeners_mutex);
            listeners.clear();
        }

        [[nodiscard]] std::size_t listener_count() const override
        {
            std::lock_guard lock(listeners_mutex);
            return listeners.size();
        }

        private:

        struct listener_entry
        {
            listener_id id{0};
            std::shared_ptr<on_change> callback;
        };

        void notify(const value_type& old_value, const value_type& new_value) const
        {
            std::vector<listener_entry> snapshot;
            {
                std::lock_guard lock(listeners_mutex);
                snapshot = listeners;
            }
            for (const listener_entry& entry : snapshot)
            {
                (*entry.callback)(old_value, new_value);
            }
        }

        static inline std::atomic<listener_id> next_listener_id{0};

        std::atomic<std::shared_ptr<const value_type>> value;
        std::mutex write_mutex;
        mutable std::mutex listeners_mutex;
        std::vector<listener_entry> listeners;
    };

    class config final
    {
        public:

        using visit_fn = std::function<void(const config_var_base::shared&)>;

        config() = delete;

        template <typename value_type>
        [[nodiscard]] static std::shared_ptr<config_var<value_type>> lookup(
            std::string_view name, const value_type& default_value, std::string_view description = {})
        {
            const config_var_base::shared base = lookup_or_create(
                name, pants::type_name<value_type>(),
                [&default_value, description](std::string normalized_name) -> config_var_base::shared {
                    return std::make_shared<config_var<value_type>>(std::move(normalized_name), default_value,
                                                                    std::string(description));
                });

            if (!base)
            {
                return nullptr;
            }

            if (base->type_name() != pants::type_name<value_type>())
            {
                utility::log_error(std::source_location::current(),
                                   "config: '{}' already registered as '{}'", name, base->type_name());
                return nullptr;
            }

            return std::static_pointer_cast<config_var<value_type>>(base);
        }

        [[nodiscard]] static config_var_base::shared lookup_base(std::string_view name);
        static void load_string(std::string_view text);
        static void load_file(std::string_view path);
        static void visit(const visit_fn& callback);
        static void clear_all_listeners();
        [[nodiscard]] static bool is_valid_name(std::string_view name) noexcept;
        [[nodiscard]] static std::string normalize_name(std::string_view name);
        [[nodiscard]] static std::size_t size();

        private:

        using create_fn = std::function<config_var_base::shared(std::string)>;

        [[nodiscard]] static config_var_base::shared lookup_or_create(std::string_view name,
                                                                      std::string_view type,
                                                                      const create_fn& create);
    };
}
