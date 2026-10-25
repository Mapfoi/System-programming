cmake_minimum_required(VERSION 3.20)
project(Lab4Registry C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(Lab4Registry main.c)

if (MSVC)
    target_compile_options(Lab4Registry PRIVATE /W3 /utf-8)
else ()
    target_compile_options(Lab4Registry PRIVATE -Wall -Wextra)
endif ()

target_link_libraries(Lab4Registry PRIVATE advapi32 kernel32)
