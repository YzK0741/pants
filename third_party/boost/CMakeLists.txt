# vendor 自 Boost 1.92.0 的 boost::context（头文件取自 MSYS2 clang64 的同版本安装，
# 源码取自 boostorg/context 的 boost-1.92.0 标签）。
# 采用默认的 fcontext 后端，目前只接入 Linux x86-64 的 ELF 汇编。

enable_language(ASM)

add_library(boost_context STATIC
        libs/context/src/fcontext.cpp
        libs/context/src/posix/stack_traits.cpp
        libs/context/src/asm/make_x86_64_sysv_elf_gas.S
        libs/context/src/asm/jump_x86_64_sysv_elf_gas.S
        libs/context/src/asm/ontop_x86_64_sysv_elf_gas.S
)

target_include_directories(boost_context PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(boost_context PRIVATE BOOST_CONTEXT_SOURCE)

find_package(Threads REQUIRED)
target_link_libraries(boost_context PUBLIC Threads::Threads)
