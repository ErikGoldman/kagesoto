cmake_minimum_required(VERSION 3.16)

project(Ashiato
    VERSION 0.1.0
    DESCRIPTION "Simple Ashiato ECS library using paged sparse arrays"
    LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ashiato_is_top_level ON)
else()
    set(ashiato_is_top_level OFF)
endif()

if(ashiato_is_top_level)
    include(CTest)
    set(ashiato_build_testing_default ${BUILD_TESTING})
else()
    set(ashiato_build_testing_default OFF)
endif()

option(ASHIATO_BUILD_EXAMPLE "Build the example executable" ${ashiato_is_top_level})
option(ASHIATO_BUILD_TESTING "Build Ashiato ECS tests" ${ashiato_build_testing_default})
option(ASHIATO_BUILD_BENCHMARKS "Build Google Benchmark suites" OFF)
option(ASHIATO_BUILD_STATIC_LIBRARY_ONLY "Build only the Ashiato ECS static library target" OFF)
option(ASHIATO_ENABLE_COVERAGE "Build Ashiato ECS targets with coverage instrumentation" OFF)
option(ASHIATO_ENABLE_DEBUG_SERVER "Build the optional Ashiato ECS local debug GraphQL server" OFF)
option(ASHIATO_INSTALL "Install Ashiato ECS library and CMake package files" ${ashiato_is_top_level})

if(ASHIATO_BUILD_STATIC_LIBRARY_ONLY)
    set(ASHIATO_BUILD_EXAMPLE OFF CACHE BOOL "Build the example executable" FORCE)
    set(ASHIATO_BUILD_TESTING OFF CACHE BOOL "Build Ashiato ECS tests" FORCE)
    set(ASHIATO_BUILD_BENCHMARKS OFF CACHE BOOL "Build Google Benchmark suites" FORCE)
endif()

if(ASHIATO_ENABLE_COVERAGE AND NOT ASHIATO_BUILD_TESTING)
    message(FATAL_ERROR "ASHIATO_ENABLE_COVERAGE requires ASHIATO_BUILD_TESTING=ON")
endif()

set(ashiato_public_headers
    include/ashiato/bit_buffer.hpp
    include/ashiato/ashiato.hpp
)

set(ashiato_sources
    src/ashiato.cpp
    src/groups.cpp
    src/jobs.cpp
    src/snapshot.cpp
    src/storage.cpp
)

if(ASHIATO_ENABLE_DEBUG_SERVER)
    list(APPEND ashiato_public_headers
        include/ashiato/debug_server.hpp
    )
    list(APPEND ashiato_sources
        src/debug_server.cpp
    )
endif()

set(ashiato_test_sources
    src/ashiato_bit_buffer.test.cpp
    src/ashiato_groups.test.cpp
    src/ashiato_job_callbacks.test.cpp
    src/ashiato_job_graph.test.cpp
    src/ashiato_job_threading.test.cpp
    src/ashiato_jobs_basic.test.cpp
    src/ashiato_metadata.test.cpp
    src/ashiato_registry.test.cpp
    src/ashiato_snapshot.test.cpp
    src/ashiato_storage.test.cpp
    src/ashiato_views_basic.test.cpp
    src/ashiato_views_iteration.test.cpp
)

if(ASHIATO_ENABLE_DEBUG_SERVER)
    list(APPEND ashiato_test_sources
        src/ashiato_debug_server.test.cpp
    )
endif()

add_library(ashiato STATIC)

target_sources(ashiato
    PRIVATE
        ${ashiato_sources}
        ${ashiato_public_headers}
)

add_library(ashiato::ashiato ALIAS ashiato)

target_compile_features(ashiato PUBLIC cxx_std_17)

if(ASHIATO_ENABLE_DEBUG_SERVER)
    target_compile_definitions(ashiato PUBLIC ASHIATO_ENABLE_DEBUG_SERVER=1)
endif()

target_include_directories(ashiato
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(ashiato PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(ASHIATO_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ASHIATO_ENABLE_COVERAGE requires GCC or Clang")
    endif()

    function(ashiato_enable_coverage target_name)
        target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
        target_link_options(${target_name} PRIVATE --coverage)
    endfunction()

    ashiato_enable_coverage(ashiato)
endif()

if(MSVC)
    target_compile_options(ashiato PRIVATE /W4)
else()
    target_compile_options(ashiato PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ASHIATO_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS ashiato
        EXPORT ashiatoTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    set(ashiato_package_config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/ashiato)

    install(EXPORT ashiatoTargets
        FILE ashiatoTargets.cmake
        NAMESPACE ashiato::
        DESTINATION ${ashiato_package_config_dir}
    )

    configure_package_config_file(
        cmake/AshiatoConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/AshiatoConfig.cmake
        INSTALL_DESTINATION ${ashiato_package_config_dir}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/AshiatoConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/AshiatoConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/AshiatoConfigVersion.cmake
        DESTINATION ${ashiato_package_config_dir}
    )
endif()

if(ASHIATO_BUILD_EXAMPLE)
    add_subdirectory(examples)
endif()

if(ASHIATO_BUILD_TESTING OR ASHIATO_BUILD_BENCHMARKS)
    include(FetchContent)
endif()

if(ASHIATO_BUILD_TESTING)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )

    FetchContent_MakeAvailable(Catch2)

    include(Catch)

    add_executable(ashiato_tests)

    target_sources(ashiato_tests
        PRIVATE
            ${ashiato_test_sources}
    )

    target_link_libraries(ashiato_tests
        PRIVATE
            ashiato::ashiato
            Catch2::Catch2WithMain
    )

    if(ASHIATO_ENABLE_COVERAGE)
        ashiato_enable_coverage(ashiato_tests)

        find_program(ASHIATO_GCOV_EXECUTABLE gcov REQUIRED)

        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
            COMMAND ${CMAKE_COMMAND}
                -DASHIATO_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DASHIATO_BINARY_DIR=${CMAKE_BINARY_DIR}
                -DASHIATO_GCOV_EXECUTABLE=${ASHIATO_GCOV_EXECUTABLE}
                -DASHIATO_COVERAGE_OUTPUT_DIR=${CMAKE_BINARY_DIR}/coverage
                -P ${CMAKE_SOURCE_DIR}/cmake/ashiato_coverage_report.cmake
            DEPENDS ashiato_tests
            USES_TERMINAL
            COMMENT "Running tests and generating gcov coverage stats"
        )
    endif()

    add_custom_target(tests DEPENDS ashiato_tests)

    catch_discover_tests(ashiato_tests)
endif()

if(ASHIATO_BUILD_BENCHMARKS)
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
