cmake_minimum_required(VERSION 3.20)
project(Lab3Queue C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(Lab3Queue main.c)

if (MSVC)
    target_compile_options(Lab3Queue PRIVATE /W3 /utf-8)
else ()
    target_compile_options(Lab3Queue PRIVATE -Wall -Wextra)
endif ()

target_link_libraries(Lab3Queue PRIVATE kernel32)

add_executable(Lab3QueueLive WIN32 window.c)

if (MSVC)
    target_compile_options(Lab3QueueLive PRIVATE /W3 /utf-8)
else ()
    target_compile_options(Lab3QueueLive PRIVATE -Wall -Wextra)
endif ()

target_link_libraries(Lab3QueueLive PRIVATE user32 gdi32 kernel32)
