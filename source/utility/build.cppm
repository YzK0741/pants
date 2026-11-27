//
// Created by 23530 on 2026/11/26.
//
module;

#include <cstddef>

export module utility:build;

export namespace utility {

#ifdef NDEBUG
inline constexpr bool debug_enabled = false;
#else
inline constexpr bool debug_enabled = true;
#endif

// 日志队列上限，超过后丢弃新日志并返回 queue_full
inline constexpr std::size_t log_queue_capacity = 8192;

}
