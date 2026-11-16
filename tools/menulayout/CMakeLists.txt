cmake_minimum_required(VERSION 3.25)
project(menulayout LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The menu layout files (.pmb) as source, offline (decompile, compile, round
# trips; docs/menu_layouts.md).  The game and the launcher read the .pmb
# files and do not use this code.
add_executable(menulayout main.cpp menu_layout.cpp)
if(MSVC)
    target_compile_definitions(menulayout PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(menulayout PRIVATE /utf-8 /W3)
endif()
