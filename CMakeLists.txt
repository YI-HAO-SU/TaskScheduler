cmake_minimum_required(VERSION 3.20)
project(TaskScheduler VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler flags
add_compile_options(-Wall -Wextra -O2)

# Library
add_library(task_scheduler
    src/thread_pool.cpp
    src/task_graph.cpp
)
target_include_directories(task_scheduler PUBLIC include)
target_link_libraries(task_scheduler PRIVATE pthread)

# Example executable
add_executable(example examples/main.cpp)
target_link_libraries(example PRIVATE task_scheduler)

# Benchmark
add_executable(benchmark benchmarks/benchmark.cpp)
target_link_libraries(benchmark PRIVATE task_scheduler)

# Tests (optional: integrate with GoogleTest)
enable_testing()
add_executable(tests tests/test_all.cpp)
target_link_libraries(tests PRIVATE task_scheduler)
add_test(NAME all_tests COMMAND tests)
