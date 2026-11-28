cmake_minimum_required(VERSION 4.4)
project(pants)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

add_executable(pants main.cpp)


if (NOT WIN32)
    add_subdirectory(third_party/mimalloc)
endif ()

add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)


add_library(base)
target_sources(base PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/base/base.cppm
)

if (NOT WIN32)
    enable_language(ASM)

    add_library(fiber)
    target_sources(fiber PUBLIC
            FILE_SET CXX_MODULES
            FILES
            source/fiber/fiber.cppm
    )
    target_sources(fiber PRIVATE
            source/fiber/fiber.cpp
            third_party/fcontext/asm/make_x86_64_sysv_elf_gas.S
            third_party/fcontext/asm/jump_x86_64_sysv_elf_gas.S
    )
    target_link_libraries(fiber PRIVATE log)
endif ()

add_library(scheduler)
target_sources(scheduler PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/scheduler/scheduler.cppm
)
target_sources(scheduler PRIVATE
        source/scheduler/scheduler.cpp
)
target_link_libraries(scheduler PUBLIC fiber PRIVATE base log)
add_library(timer)
target_sources(timer PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/timer/timer.cppm
)
target_sources(timer PRIVATE
        source/timer/timer.cpp
)
target_link_libraries(timer PRIVATE log)

add_library(iomanager)
target_sources(iomanager PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/iomanager/iomanager.cppm
)
target_sources(iomanager PRIVATE
        source/iomanager/iomanager.cpp
)
target_link_libraries(iomanager PUBLIC scheduler timer PRIVATE fiber log)

if (NOT WIN32)
    add_library(config)
    target_sources(config PUBLIC
            FILE_SET CXX_MODULES
            FILES
            source/config/config.cppm
    )
    target_sources(config PRIVATE
            source/config/config.cpp
    )
    target_include_directories(config PRIVATE third_party/toml)
    target_link_libraries(config PRIVATE log)
endif ()

add_library(mutex)
target_sources(mutex PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/mutex/mutex.cppm
)

add_library(fd_manager)
target_sources(fd_manager PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/fd_manager/fd_manager.cppm
)
target_sources(fd_manager PRIVATE
        source/fd_manager/fd_manager.cpp
)

if (NOT WIN32)
    add_library(hook)
    target_sources(hook PUBLIC
            FILE_SET CXX_MODULES
            FILES
            source/hook/hook.cppm
    )
    target_sources(hook PRIVATE
            source/hook/hook.cpp
    )
    target_link_libraries(hook PUBLIC iomanager fd_manager PRIVATE fiber timer log ${CMAKE_DL_LIBS})
endif ()

add_library(log)
target_sources(log PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/log/log.cppm
        source/log/log_core.cppm
)
target_link_libraries(log PUBLIC base)

add_library(utility)
target_sources(utility PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/utility/utility.cppm
        source/utility/text/text.cppm
        source/utility/filesystem/filesystem.cppm
        source/utility/diagnostics/diagnostics.cppm
        source/utility/thread/thread.cppm
)
target_sources(utility PRIVATE
        source/utility/text/text.cpp
        source/utility/filesystem/filesystem.cpp
        source/utility/diagnostics/diagnostics.cpp
        source/utility/thread/thread.cpp
)
target_link_libraries(utility PUBLIC base log)

target_link_libraries(pants PUBLIC utility)

if (NOT WIN32)
    add_library(memory)
    target_sources(memory PRIVATE
            source/memory/alloc.cpp
    )
    target_link_libraries(memory PRIVATE mimalloc_vendored)
    target_link_libraries(memory PUBLIC base)
    target_link_libraries(pants PRIVATE memory)
endif ()


include(CTest)
if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
