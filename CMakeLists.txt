cmake_minimum_required(VERSION 3.25)
project(gt3pc LANGUAGES C CXX)

# 32-bit only: the game's data files carry 32-bit pointers that the game
# relocates in place (models, textures, menus, sound tables).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GT3 PC builds as 32-bit x86: configure from the x86 MSVC environment (build.cmd / tools\\msvc.cmd)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the executables need no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_options(/utf-8 /W3 /wd4244 /wd4267 /wd4305 /wd4996)

set(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(EXISTS ${GT3PC_ROOT}/src/CMakeLists.txt)
    add_subdirectory(src)
endif()

# Test and development tools: one folder each under tools/ with its own
# CMakeLists.txt (tools/<name>/CMakeLists.txt), each also buildable alone.
option(GT3PC_BUILD_TOOLS "Build the test tools of tools/ too" OFF)
if(GT3PC_BUILD_TOOLS)
    file(GLOB GT3PC_TOOL_LISTS CONFIGURE_DEPENDS ${GT3PC_ROOT}/tools/*/CMakeLists.txt)
    foreach(list ${GT3PC_TOOL_LISTS})
        get_filename_component(dir ${list} DIRECTORY)
        add_subdirectory(${dir})
    endforeach()
endif()
