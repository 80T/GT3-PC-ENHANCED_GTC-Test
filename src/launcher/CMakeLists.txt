# gt3launcher.exe: the launcher (configure, install the game files from the
# user's disc image, start gt3pc.exe).  Plain Win32 and the Windows SDK, with
# the game's control mappings (src/platform/input_map.cpp) and SDL3,
# delay-loaded as in the game, for the Controls tab's capture of gamepad and
# wheel inputs (cmake/sdl3.cmake).
# Included from src/CMakeLists.txt; also buildable on its own:
#   tools\msvc.cmd cmake -S src/launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(gt3launcher LANGUAGES CXX RC)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options(/utf-8 /W3)
    set(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()
if(NOT TARGET gt3_sdl3)
    include(${GT3PC_ROOT}/cmake/sdl3.cmake)
endif()

add_executable(gt3launcher WIN32
    main.cpp
    ui.cpp
    panel.cpp
    controls.cpp
    install.cpp
    display.cpp
    inflate.cpp
    iso9660.cpp
    util.cpp
    volume.cpp
    ${GT3PC_ROOT}/src/platform/input_map.cpp
    gt3launcher.rc
    gt3launcher.manifest
)
target_compile_definitions(gt3launcher PRIVATE UNICODE _UNICODE)
target_include_directories(gt3launcher PRIVATE ${GT3PC_ROOT}/src)
target_link_libraries(gt3launcher PRIVATE comctl32 comdlg32 shell32 ole32 windowscodecs gt3_sdl3)
add_custom_command(TARGET gt3launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GT3PC_SDL3_DLL} $<TARGET_FILE_DIR:gt3launcher>)
# (the background art: a resource, decoded by WIC)
set_property(SOURCE gt3launcher.rc APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../launcher_bg.png ${CMAKE_CURRENT_SOURCE_DIR}/../../gt3pc.ico)
# Small: no incremental-link padding, unreferenced code dropped (the .pdb
# keeps the debug information).
target_link_options(gt3launcher PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF)
