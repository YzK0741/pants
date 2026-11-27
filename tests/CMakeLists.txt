add_executable(test_thread_context test_thread_context.cpp)
target_link_libraries(test_thread_context PRIVATE base)
add_test(NAME thread_context COMMAND test_thread_context)

add_executable(test_log_basic test_log_basic.cpp)
target_link_libraries(test_log_basic PRIVATE utility)
add_test(NAME log_basic COMMAND test_log_basic)

if (NOT WIN32)
    add_executable(test_panic test_panic.cpp)
    target_link_libraries(test_panic PRIVATE base)
    add_test(NAME panic_aborts COMMAND test_panic)
endif ()
