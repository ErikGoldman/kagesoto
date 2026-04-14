cmake_minimum_required(VERSION 3.16)

project(SimpleLibrary
    VERSION 0.1.0
    DESCRIPTION "Minimal cross-platform C++ library scaffold"
    LANGUAGES CXX
)

include(CTest)

option(SIMPLELIB_BUILD_EXAMPLE "Build the example executable" ON)

add_library(simplelib
    src/simplelib.cpp
)

add_library(simplelib::simplelib ALIAS simplelib)

target_compile_features(simplelib PUBLIC cxx_std_17)

target_include_directories(simplelib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(simplelib
    PRIVATE SIMPLELIB_BUILDING_LIBRARY
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(simplelib PUBLIC SIMPLELIB_STATIC)
endif()

set_target_properties(simplelib PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(MSVC)
    target_compile_options(simplelib PRIVATE /W4)
else()
    target_compile_options(simplelib PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SIMPLELIB_BUILD_EXAMPLE)
    add_executable(simplelib_example
        examples/main.cpp
    )

    target_link_libraries(simplelib_example PRIVATE simplelib::simplelib)
endif()

if(BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )

    FetchContent_MakeAvailable(Catch2)

    include(Catch)

    add_executable(simplelib_tests
        tests/simplelib_tests.cpp
    )

    target_link_libraries(simplelib_tests
        PRIVATE
            simplelib::simplelib
            Catch2::Catch2WithMain
    )

    add_custom_target(tests DEPENDS simplelib_tests)

    catch_discover_tests(simplelib_tests)
endif()
