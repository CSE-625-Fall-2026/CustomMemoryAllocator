add_executable(custom_memory_allocator_tests MemoryPoolTests.cpp)
target_link_libraries(
    custom_memory_allocator_tests
    PRIVATE CustomMemoryAllocator::core
)

add_test(
    NAME custom_memory_allocator_tests
    COMMAND custom_memory_allocator_tests
)

add_executable(global_new_tests GlobalNewTests.cpp)
target_link_libraries(
    global_new_tests
    PRIVATE CustomMemoryAllocator::global_new
)

add_test(NAME global_new_tests COMMAND global_new_tests)
