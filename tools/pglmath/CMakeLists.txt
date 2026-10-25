cmake_minimum_required(VERSION 3.25)
project(pglmath LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
add_executable(pglmath main.cpp
    ${GT3PC}/src/engine/pgl/native/pgl_math.cpp
    ${GT3PC}/src/engine/pgl/native/pgl_state_gs.cpp
    ${GT3PC}/src/engine/pgl/native/pgl_state_prim.cpp)
target_include_directories(pglmath PRIVATE ${GT3PC}/src)
if(MSVC)
    # The VU ACC chains are written out operation by operation: no
    # contraction into fused multiply-adds.
    target_compile_options(pglmath PRIVATE /fp:precise)
endif()
