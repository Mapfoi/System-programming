cmake_minimum_required(VERSION 3.20)
project(Lab5Dll C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_library(memreplace SHARED memreplace_dll.c)
set_target_properties(memreplace PROPERTIES
    PREFIX ""
    OUTPUT_NAME "memreplace"
)

if (MSVC)
    target_compile_options(memreplace PRIVATE /W3 /utf-8)
else ()
    target_compile_options(memreplace PRIVATE -Wall -Wextra)
endif ()

target_link_libraries(memreplace PRIVATE kernel32)

add_executable(Lab5Static static_import.c)
add_executable(Lab5Dynamic dynamic_import.c)
add_executable(Lab5Target target_host.c)
add_executable(Lab5Inject inject_remote.c)

foreach (t Lab5Static Lab5Dynamic Lab5Target Lab5Inject)
    if (MSVC)
        target_compile_options(${t} PRIVATE /W3 /utf-8)
    else ()
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif ()
endforeach ()

target_link_libraries(Lab5Static PRIVATE memreplace kernel32)
target_link_libraries(Lab5Dynamic PRIVATE kernel32)
target_link_libraries(Lab5Target PRIVATE kernel32)
target_link_libraries(Lab5Inject PRIVATE kernel32)

add_dependencies(Lab5Static memreplace)
add_dependencies(Lab5Dynamic memreplace)
add_dependencies(Lab5Inject memreplace)
