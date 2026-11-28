#ifndef PANTS_SOURCE_FIBER_FCONTEXT_ASM_H
#define PANTS_SOURCE_FIBER_FCONTEXT_ASM_H

#include <cstddef>

// 与 third_party/fcontext/asm/*.S 约定的 ABI（来自 Boost.Context 的 fcontext）。
struct fcontext_transfer_t
{
    void* fctx{nullptr};
    void* data{nullptr};
};

extern "C"
{
    void* make_fcontext(void* sp, std::size_t size, fcontext_transfer_t (*fn)(fcontext_transfer_t));
    fcontext_transfer_t jump_fcontext(void* to, void* vp);
}

#endif
