cmake_minimum_required(VERSION 3.20)
project(ModelingGravitationalInteraction C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Единая директория вывода, чтобы gravity_sim.exe находил gravity_physics.dll при LoadLibraryW
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# 1. Динамическая библиотека физического ядра (gravity_physics.dll)
add_library(gravity_physics SHARED physics_dll.c)
set_target_properties(gravity_physics PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gravity_physics"
)

if (MSVC)
    target_compile_options(gravity_physics PRIVATE /W3 /utf-8)
else ()
    target_compile_options(gravity_physics PRIVATE -Wall -Wextra -O2)
endif ()

target_link_libraries(gravity_physics PRIVATE kernel32)

# 2. Основное Win32 GDI-приложение (gravity_sim.exe)
add_executable(gravity_sim WIN32 main_win.c storage_sys.c)
add_dependencies(gravity_sim gravity_physics)

if (MSVC)
    target_compile_options(gravity_sim PRIVATE /W3 /utf-8)
else ()
    target_compile_options(gravity_sim PRIVATE -Wall -Wextra -O2)
endif ()

target_link_libraries(gravity_sim PRIVATE user32 gdi32 kernel32 advapi32)
