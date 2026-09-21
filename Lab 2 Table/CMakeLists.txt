cmake_minimum_required(VERSION 3.20)
project(Lab2Table C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(Lab2Table WIN32 main.c)

target_compile_definitions(Lab2Table PRIVATE UNICODE _UNICODE)

if (MSVC)
    target_compile_options(Lab2Table PRIVATE /W3)
else ()
    target_compile_options(Lab2Table PRIVATE -Wall -Wextra)
endif ()

target_link_libraries(Lab2Table PRIVATE user32 gdi32)

# MinGW по умолчанию ищет WinMain; -municode — точку входа wWinMain
if (MINGW)
    target_link_options(Lab2Table PRIVATE -municode)
endif ()
