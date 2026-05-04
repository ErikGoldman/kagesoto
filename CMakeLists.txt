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

add_library(ecs)

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
    PRIVATE
        ECS_BUILDING_LIBRARY
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
