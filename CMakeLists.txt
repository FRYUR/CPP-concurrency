cmake_minimum_required(VERSION 3.21)

project(TaskForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)

set(TASKFORGE_SANITIZER "none" CACHE STRING "Sanitizer: none, address, or thread")
set_property(CACHE TASKFORGE_SANITIZER PROPERTY STRINGS none address thread)
option(TASKFORGE_BUILD_BENCHMARKS "Build optimized Google Benchmark targets" OFF)

add_library(taskforge src/thread_pool.cpp src/work_stealing_pool.cpp)
add_library(TaskForge::taskforge ALIAS taskforge)
target_include_directories(taskforge PUBLIC include)
target_compile_features(taskforge PUBLIC cxx_std_20)
target_link_libraries(taskforge PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(taskforge PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TASKFORGE_SANITIZER STREQUAL "address")
    set(taskforge_sanitizer_flag address)
elseif(TASKFORGE_SANITIZER STREQUAL "thread")
    set(taskforge_sanitizer_flag thread)
elseif(NOT TASKFORGE_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "TASKFORGE_SANITIZER must be none, address, or thread")
endif()

if(taskforge_sanitizer_flag)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "TaskForge sanitizers require a Clang or GCC toolchain")
    endif()
    target_compile_options(taskforge PUBLIC
        "-fsanitize=${taskforge_sanitizer_flag}"
        -fno-omit-frame-pointer
    )
    target_link_options(taskforge PUBLIC "-fsanitize=${taskforge_sanitizer_flag}")
endif()

add_executable(taskforge_independent_tasks examples/independent_tasks.cpp)
target_link_libraries(taskforge_independent_tasks PRIVATE TaskForge::taskforge)

add_executable(taskforge_uneven_cpu_tasks examples/uneven_cpu_tasks.cpp)
target_link_libraries(taskforge_uneven_cpu_tasks PRIVATE TaskForge::taskforge)

add_executable(taskforge_parallel_reduction examples/parallel_reduction.cpp)
target_link_libraries(taskforge_parallel_reduction PRIVATE TaskForge::taskforge)

if(TASKFORGE_BUILD_BENCHMARKS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "Benchmarks must be built with CMAKE_BUILD_TYPE=Release")
    endif()
    if(NOT TASKFORGE_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "Benchmarks must be built without sanitizers")
    endif()

    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(taskforge_benchmarks benchmarks/scheduler_benchmarks.cpp)
    target_link_libraries(taskforge_benchmarks
        PRIVATE TaskForge::taskforge benchmark::benchmark_main
    )
endif()

if(BUILD_TESTING)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(taskforge_tests
        tests/thread_pool_test.cpp
        tests/work_stealing_pool_test.cpp
    )
    target_link_libraries(taskforge_tests PRIVATE TaskForge::taskforge GTest::gtest_main Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(taskforge_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME taskforge_tests COMMAND taskforge_tests)
    set_tests_properties(taskforge_tests PROPERTIES TIMEOUT 20)
    add_test(NAME taskforge_independent_tasks COMMAND taskforge_independent_tasks)
    add_test(NAME taskforge_uneven_cpu_tasks COMMAND taskforge_uneven_cpu_tasks)
    add_test(NAME taskforge_parallel_reduction COMMAND taskforge_parallel_reduction)
    set_tests_properties(
        taskforge_independent_tasks
        taskforge_uneven_cpu_tasks
        taskforge_parallel_reduction
        PROPERTIES TIMEOUT 20
    )
endif()
