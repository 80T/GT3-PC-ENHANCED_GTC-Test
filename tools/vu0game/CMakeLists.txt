cmake_minimum_required(VERSION 3.25)
project(vu0game LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
add_executable(vu0game main.cpp
    ${GT3PC}/src/engine/pgl/native/pgl_math.cpp)
target_include_directories(vu0game PRIVATE ${GT3PC}/src ${GT3PC}/include)
if(MSVC)
    # The VU ACC chains are written out operation by operation: no
    # contraction into fused multiply-adds.
    target_compile_options(vu0game PRIVATE /fp:precise)
endif()
