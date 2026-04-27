cmake_minimum_required(VERSION 3.16)

project(ECS
    VERSION 0.1.0
    DESCRIPTION "Simple ECS library using paged sparse arrays"
    LANGUAGES CXX
)

include(CTest)

option(ECS_BUILD_EXAMPLE "Build the example executable" ON)
option(ECS_BUILD_BENCHMARKS "Build Google Benchmark suites" OFF)
option(ECS_BUILD_PROFILING "Build benchmark-friendly profiling instrumentation" OFF)
set(ECS_INDEX_BACKEND "flat_sorted" CACHE STRING "Internal component index backend")
set_property(CACHE ECS_INDEX_BACKEND PROPERTY STRINGS optimized_bplus flat_sorted)

if(ECS_INDEX_BACKEND STREQUAL "optimized_bplus")
    set(ECS_INDEX_BACKEND_VALUE 1)
elseif(ECS_INDEX_BACKEND STREQUAL "flat_sorted")
    set(ECS_INDEX_BACKEND_VALUE 2)
else()
    message(FATAL_ERROR "ECS_INDEX_BACKEND must be one of: optimized_bplus, flat_sorted")
endif()

file(GLOB_RECURSE ecs_public_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hxx"
)

file(GLOB_RECURSE ecs_all_src CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

set(ecs_test_sources ${ecs_all_src})
list(FILTER ecs_test_sources INCLUDE REGEX [[\.test\.(c|cc|cpp|cxx)$]])

set(ecs_sources ${ecs_all_src})
list(FILTER ecs_sources EXCLUDE REGEX [[\.test\.(c|cc|cpp|cxx)$]])

add_library(ecs
        src/registry.hpp)

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

target_compile_definitions(ecs
    PRIVATE ECS_BUILDING_LIBRARY
    PUBLIC
        ECS_INDEX_BACKEND=${ECS_INDEX_BACKEND_VALUE}
        ECS_INDEX_BACKEND_NAME="${ECS_INDEX_BACKEND}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(ecs PUBLIC ECS_STATIC)
endif()

set_target_properties(ecs PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(MSVC)
    target_compile_options(ecs PRIVATE /W4)
else()
    target_compile_options(ecs PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ECS_BUILD_PROFILING)
    target_compile_definitions(ecs PUBLIC ECS_ENABLE_PROFILING=1)
    if(MSVC)
        target_compile_options(ecs PRIVATE /Zi)
    else()
        target_compile_options(ecs PRIVATE -g -fno-omit-frame-pointer)
    endif()
endif()

if(ECS_BUILD_EXAMPLE)
    add_subdirectory(examples)
endif()

if(BUILD_TESTING OR ECS_BUILD_BENCHMARKS)
    include(FetchContent)
endif()

if(BUILD_TESTING)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )

    FetchContent_MakeAvailable(Catch2)

    include(Catch)

    add_executable(ecs_tests
            src/registry.hpp)

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

    if(ECS_BUILD_PROFILING)
        FetchContent_Declare(
            tracy
            GIT_REPOSITORY https://github.com/wolfpld/tracy.git
            GIT_TAG v0.11.1
        )

        FetchContent_MakeAvailable(tracy)

        if(TARGET TracyClient)
            target_link_libraries(ecs PRIVATE TracyClient)
            target_compile_definitions(ecs PUBLIC TRACY_ENABLE=1)
        else()
            message(FATAL_ERROR "ECS_BUILD_PROFILING=ON requires TracyClient target from tracy")
        endif()

        if(NOT TARGET tracy-capture)
            set(NO_FILESELECTOR ON CACHE BOOL "" FORCE)
            add_subdirectory("${tracy_SOURCE_DIR}/capture" "${tracy_BINARY_DIR}/capture")
        endif()
    endif()

    add_subdirectory(benchmarks)
endif()
