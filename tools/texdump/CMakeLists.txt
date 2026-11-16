cmake_minimum_required(VERSION 3.25)
project(texdump LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

# The texture decoder library (src/engine/pgl/native/gs_tex*): plain C++,
# no Windows or game dependency.
set(GS_TEX_SOURCES
    ${GT3PC}/src/engine/pgl/native/gs_tex_format.cpp
    ${GT3PC}/src/engine/pgl/native/gs_tex_layout.cpp
    ${GT3PC}/src/engine/pgl/native/gs_tex_space.cpp
    ${GT3PC}/src/engine/pgl/native/gs_tex_decode.cpp
    ${GT3PC}/src/engine/pgl/native/gs_tex1.cpp
    ${GT3PC}/src/engine/pgl/native/gs_imgs.cpp)

add_executable(texdump main.cpp png_write.cpp inflate.cpp ${GS_TEX_SOURCES})
target_include_directories(texdump PRIVATE ${GT3PC}/src)
if(MSVC)
    target_compile_definitions(texdump PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(texdump PRIVATE /utf-8 /W3)
endif()
