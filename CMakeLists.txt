cmake_minimum_required(VERSION 3.16)

project(ECS
    VERSION 0.1.0
    DESCRIPTION "Simple ECS library using paged sparse arrays"
    LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ecs_is_top_level ON)
else()
    set(ecs_is_top_level OFF)
endif()

if(ecs_is_top_level)
    include(CTest)
    set(ecs_build_testing_default ${BUILD_TESTING})
else()
    set(ecs_build_testing_default OFF)
endif()

option(ECS_BUILD_EXAMPLE "Build the example executable" ${ecs_is_top_level})
option(ECS_BUILD_TESTING "Build ECS tests" ${ecs_build_testing_default})
option(ECS_BUILD_BENCHMARKS "Build Google Benchmark suites" OFF)

set(ecs_public_headers
    include/ecs/bit_buffer.hpp
    include/ecs/ecs.hpp
)

set(ecs_sources
    src/ecs.cpp
    src/groups.cpp
    src/jobs.cpp
    src/snapshot.cpp
    src/storage.cpp
)

set(ecs_test_sources
    src/ecs_bit_buffer.test.cpp
    src/ecs_groups.test.cpp
    src/ecs_job_callbacks.test.cpp
    src/ecs_job_graph.test.cpp
    src/ecs_job_threading.test.cpp
    src/ecs_jobs_basic.test.cpp
    src/ecs_metadata.test.cpp
    src/ecs_registry.test.cpp
    src/ecs_snapshot.test.cpp
    src/ecs_storage.test.cpp
    src/ecs_views_basic.test.cpp
    src/ecs_views_iteration.test.cpp
)

add_library(ecs STATIC)

target_sources(ecs
    PRIVATE
        ${ecs_sources}
        ${ecs_public_headers}
)

add_library(ecs::ecs ALIAS ecs)

target_compile_features(ecs PUBLIC cxx_std_17)

target_include_directories(ecs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(ecs PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(MSVC)
    target_compile_options(ecs PRIVATE /W4)
else()
    target_compile_options(ecs PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ECS_BUILD_EXAMPLE)
    add_subdirectory(examples)
endif()

if(ECS_BUILD_TESTING OR ECS_BUILD_BENCHMARKS)
    include(FetchContent)
endif()

if(ECS_BUILD_TESTING)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )

    FetchContent_MakeAvailable(Catch2)

    include(Catch)

    add_executable(ecs_tests)

    target_sources(ecs_tests
        PRIVATE
            ${ecs_test_sources}
    )

    target_link_libraries(ecs_tests
        PRIVATE
            ecs::ecs
            Catch2::Catch2WithMain
    )

    add_custom_target(tests DEPENDS ecs_tests)

    catch_discover_tests(ecs_tests)
endif()

if(ECS_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        google_benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )

    FetchContent_MakeAvailable(google_benchmark)

    add_subdirectory(benchmarks)
endif()
