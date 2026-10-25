cmake_minimum_required(VERSION 3.25)
project(mdldump LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

# mdldump: the prebuilt VU1 model decoder (src/engine/pgl/native/vif_model.cpp)
# on the model files of gt3.vol; writes .obj / .mtl / statistics.  32-bit,
# as the game: it relocates the files' pointers in place.
add_executable(mdldump main.cpp ${GT3PC}/src/engine/pgl/native/vif_model.cpp)
target_include_directories(mdldump PRIVATE ${GT3PC}/src)
if(MSVC)
    target_compile_definitions(mdldump PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mdldump PRIVATE /W3 /utf-8)
endif()
