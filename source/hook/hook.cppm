//
// Created by 23530 on 2026/11/28.
//
export module hook;

export namespace pants
{
    void set_hook_enable(bool enabled) noexcept;
    [[nodiscard]] bool is_hook_enable() noexcept;
}
