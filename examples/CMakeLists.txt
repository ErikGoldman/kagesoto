add_executable(ashiato_example
    main.cpp
)

target_link_libraries(ashiato_example PRIVATE ashiato::ashiato)

find_package(X11 QUIET)
find_package(OpenGL QUIET)

set(ashiato_can_build_raylib_examples OFF)
if(X11_FOUND
        AND OpenGL_FOUND
        AND OPENGL_INCLUDE_DIR
        AND X11_Xrandr_INCLUDE_PATH
        AND X11_Xrandr_LIB
        AND X11_Xinerama_INCLUDE_PATH
        AND X11_Xinerama_LIB
        AND X11_Xcursor_INCLUDE_PATH
        AND X11_Xcursor_LIB
        AND X11_Xi_INCLUDE_PATH
        AND X11_Xi_LIB)
    set(ashiato_can_build_raylib_examples ON)
endif()

if(NOT ashiato_can_build_raylib_examples)
    message(WARNING "Skipping ashiato_balls_3d_example because raylib's X11/OpenGL development dependencies are missing")
    return()
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

include(FetchContent)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.0
)

FetchContent_MakeAvailable(raylib)

add_executable(ashiato_balls_3d_example
    balls_3d.cpp
)

target_link_libraries(ashiato_balls_3d_example
    PRIVATE
        ashiato::ashiato
        raylib
)
