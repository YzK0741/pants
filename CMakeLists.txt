cmake_minimum_required(VERSION 4.4)
project(pants)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

add_executable(pants main.cpp)


if (NOT WIN32)
    include(FetchContent)

    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(mimalloc
            GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
            GIT_TAG v3.7.1
            GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(mimalloc)
endif ()


add_library(base)
target_sources(base PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/base/base.cppm
)

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
)
target_link_libraries(utility PUBLIC base log)

target_link_libraries(pants PUBLIC utility)

if (NOT WIN32)
    add_library(memory)
    target_sources(memory PRIVATE
            source/memory/alloc.cpp
    )
    target_link_libraries(memory PRIVATE mimalloc-static)
    target_link_libraries(memory PUBLIC base)
    target_link_libraries(pants PRIVATE memory)
endif ()


include(CTest)
if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
