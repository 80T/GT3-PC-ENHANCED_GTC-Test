# The native sound back end (SPU2 model, reverb, BGM stream player, the IOP
# sound services): a static library with no Windows or game dependency.
# Included by tools/sndtest; the game build can add_subdirectory() it too
# (the target is defined once).
cmake_minimum_required(VERSION 3.25)
project(gt3_sound_native LANGUAGES CXX)

if(NOT TARGET gt3_sound_native)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    get_filename_component(GT3PC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_library(gt3_sound_native STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/spu2_adpcm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/spu2_reverb.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/spu2.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ads_stream.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sound_native.cpp)
    target_include_directories(gt3_sound_native PUBLIC ${GT3PC_SRC})
    if(MSVC)
        target_compile_options(gt3_sound_native PRIVATE /W3 /utf-8)
    endif()
endif()
