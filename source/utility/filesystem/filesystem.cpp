//
// Created by 23530 on 2026/11/28.
//
module;

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

module utility.filesystem;

import log;
import utility.text;

namespace pants
{
    void list_all_files(std::vector<std::string>& files, std::string_view path, std::string_view suffix)
    {
        if (path.empty())
        {
            return;
        }

        const std::size_t first_new_file = files.size();
        const std::filesystem::path root{path};
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error)
        {
            return;
        }

        constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
        for (auto iterator = std::filesystem::recursive_directory_iterator(root, options, error);
             iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }

            const std::filesystem::directory_entry& entry = *iterator;
            std::error_code entry_error;
            if (!entry.is_regular_file(entry_error) || entry_error)
            {
                continue;
            }

            std::string file_name = entry.path().lexically_normal().string();
            if (suffix.empty() || file_name.ends_with(suffix))
            {
                files.push_back(std::move(file_name));
            }
        }

        std::sort(files.begin() + static_cast<std::ptrdiff_t>(first_new_file), files.end());
    }

    bool mkdir(std::string_view dirname) noexcept
    {
        if (dirname.empty())
        {
            return false;
        }
        std::error_code error;
        const bool created = std::filesystem::create_directories(std::filesystem::path{dirname}, error);
        if (error)
        {
            utility::log_error(std::source_location::current(), "filesystem: cannot create '{}': {}",
                               dirname, error.message());
            return false;
        }
        return created || std::filesystem::is_directory(std::filesystem::path{dirname}, error);
    }

    bool is_running_pidfile(std::string_view pidfile) noexcept
    {
        std::ifstream input{std::string(pidfile)};
        if (!input.is_open())
        {
            return false;
        }

        std::string line;
        std::getline(input, line);

        long long pid_value = 0;
        const std::string_view trimmed = trim(line);
        const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), pid_value);
        if (result.ec != std::errc{} || pid_value <= 0)
        {
            return false;
        }

        return ::kill(static_cast<pid_t>(pid_value), 0) == 0;
    }

    bool unlink(std::string_view filename, bool exist) noexcept
    {
        if (filename.empty())
        {
            return false;
        }
        const std::string path{filename};
        if (exist && ::access(path.c_str(), F_OK) != 0)
        {
            return true;
        }
        return ::unlink(path.c_str()) == 0;
    }
}
