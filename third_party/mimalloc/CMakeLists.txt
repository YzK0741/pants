# vendor 自 deren 项目的 mimalloc 3.4.4（include + src，去掉了上游的 CMake 工程，
# 按需编译成静态库，避免 configure 阶段联网 clone）。

add_library(mimalloc_vendored STATIC
        src/alloc.c
        src/alloc-aligned.c
        src/alloc-posix.c
        src/arena.c
        src/arena-meta.c
        src/bitmap.c
        src/heap.c
        src/init.c
        src/libc.c
        src/options.c
        src/os.c
        src/page.c
        src/page-map.c
        src/random.c
        src/stats.c
        src/theap.c
        src/threadlocal.c
        src/prim/prim.c
)

target_include_directories(mimalloc_vendored SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (WIN32)
    target_compile_definitions(mimalloc_vendored PRIVATE _WIN32_WINNT=0x600)
    target_link_libraries(mimalloc_vendored PUBLIC psapi shell32 user32 advapi32 bcrypt)
endif ()
