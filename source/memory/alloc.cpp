//
// Created by 23530 on 2026/11/27.
//
// 全局 operator new / delete 替换为 mimalloc。
// 这是一个普通实现单元（不属于任何模块），因为替换全局分配函数没有模块形式。
//

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <mimalloc.h>

import base;

namespace {

[[noreturn]] void panic_oom() noexcept {
  // 不格式化、不分配：走到这里通常就是因为内存已经没了
  utility::panic("oom! fast panic");
}

[[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept {
  // C++ 允许 new(0)：标准要求返回非空指针
  const std::size_t bytes = size != 0 ? size : 1;
  if (alignment <= alignof(std::max_align_t)) {
    return mi_malloc(bytes);
  }
  return mi_malloc_aligned(bytes, alignment);
}

} // namespace

void* operator new(std::size_t size) {
  void* const memory = allocate(size, alignof(std::max_align_t));
  if (memory == nullptr) {
    panic_oom();
  }
  return memory;
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return allocate(size, alignof(std::max_align_t));
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return allocate(size, alignof(std::max_align_t));
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  void* const memory = allocate(size, static_cast<std::size_t>(alignment));
  if (memory == nullptr) {
    panic_oom();
  }
  return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept {
  mi_free(memory);
}

void operator delete[](void* memory) noexcept {
  mi_free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  mi_free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  mi_free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
  mi_free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  mi_free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
  mi_free(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
  mi_free(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  mi_free(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  mi_free(memory);
}
