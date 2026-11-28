//
// Created by 23530 on 2026/11/28.
//
module;

#include <string>
#include <string_view>
#include <vector>

export module utility.filesystem;

export namespace pants
{
    void list_all_files(std::vector<std::string>& files, std::string_view path, std::string_view suffix = {});
    [[nodiscard]] bool mkdir(std::string_view dirname) noexcept;
    [[nodiscard]] bool is_running_pidfile(std::string_view pidfile) noexcept;
    [[nodiscard]] bool unlink(std::string_view filename, bool exist = false) noexcept;
}
