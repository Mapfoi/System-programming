cmake_minimum_required(VERSION 3.20)
project(GravitySim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_library(gravity_physics SHARED physics_dll.c)
set_target_properties(gravity_physics PROPERTIES PREFIX "" OUTPUT_NAME "gravity_physics")
target_link_libraries(gravity_physics PRIVATE kernel32)

add_executable(gravity_sim WIN32 main_win.c)
add_dependencies(gravity_sim gravity_physics)
target_link_libraries(gravity_sim PRIVATE user32 gdi32 kernel32)

if (MSVC)
    target_compile_options(gravity_physics PRIVATE /W3 /utf-8)
    target_compile_options(gravity_sim PRIVATE /W3 /utf-8)
else ()
    target_compile_options(gravity_physics PRIVATE -Wall -Wextra -O2)
    target_compile_options(gravity_sim PRIVATE -Wall -Wextra -O2)
endif ()
