cmake_minimum_required(VERSION 3.25)
project(schedtest LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
add_executable(schedtest
    main.cpp
    ${GT3PC}/src/platform/scheduler.cpp
    ${GT3PC}/src/platform/ee_kernel.cpp
    ${GT3PC}/src/platform/log.cpp
)
target_include_directories(schedtest PRIVATE ${GT3PC}/include ${GT3PC}/src)
target_compile_definitions(schedtest PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(schedtest PRIVATE winmm)
