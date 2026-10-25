# SDL3 (zlib licence), the port's one third-party library: the game
# controllers (roadmap "Controller support 1", src/platform/gamepad.cpp).
# The official Visual C++ development package of a pinned release (the
# version PCSX2 ships) is downloaded once into third_party/, checked against
# its SHA-256; only the headers, the x86 import library and DLL and the
# licence are extracted.  gt3pc.exe delay-loads SDL3.dll, copied next to it,
# and runs with the keyboard alone when the DLL is missing.
set(GT3PC_SDL3_VERSION 3.2.26)
set(GT3PC_SDL3_SHA256 687C88003034FC596EF95507703E6E8AF62415F11783FE0B07A7A486FA49E1C0)
set(GT3PC_SDL3_DIR ${GT3PC_ROOT}/third_party/SDL3-${GT3PC_SDL3_VERSION})

if(NOT EXISTS ${GT3PC_SDL3_DIR}/lib/x86/SDL3.lib)
    set(zip ${CMAKE_BINARY_DIR}/SDL3-devel-${GT3PC_SDL3_VERSION}-VC.zip)
    message(STATUS "Downloading SDL3 ${GT3PC_SDL3_VERSION} for the controllers")
    file(DOWNLOAD
        https://github.com/libsdl-org/SDL/releases/download/release-${GT3PC_SDL3_VERSION}/SDL3-devel-${GT3PC_SDL3_VERSION}-VC.zip
        ${zip} EXPECTED_HASH SHA256=${GT3PC_SDL3_SHA256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "SDL3 ${GT3PC_SDL3_VERSION} could not be downloaded: ${status}")
    endif()
    set(v SDL3-${GT3PC_SDL3_VERSION})
    file(ARCHIVE_EXTRACT INPUT ${zip} DESTINATION ${GT3PC_ROOT}/third_party
        PATTERNS ${v}/include/* ${v}/lib/x86/SDL3.dll ${v}/lib/x86/SDL3.lib ${v}/LICENSE.txt ${v}/README.md)
    file(REMOVE ${zip})
endif()

add_library(gt3_sdl3 INTERFACE)
target_include_directories(gt3_sdl3 INTERFACE ${GT3PC_SDL3_DIR}/include)
target_link_libraries(gt3_sdl3 INTERFACE ${GT3PC_SDL3_DIR}/lib/x86/SDL3.lib delayimp)
target_link_options(gt3_sdl3 INTERFACE /DELAYLOAD:SDL3.dll)
set(GT3PC_SDL3_DLL ${GT3PC_SDL3_DIR}/lib/x86/SDL3.dll)
