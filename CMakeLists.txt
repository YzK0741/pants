cmake_minimum_required(VERSION 4.4)
project(pants)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

add_executable(pants main.cpp)


add_library(utility)

target_sources(utility PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/utility/utility.cppm
        source/utility/log/log.cppm
)

target_link_libraries(pants PUBLIC
    utility
)

