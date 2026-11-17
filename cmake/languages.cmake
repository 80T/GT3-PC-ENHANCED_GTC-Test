# The interface's languages (src/platform/ui_text.h, docs/languages.md "The
# interface's languages"), PC 2026-11-16.
#
# Every build first runs tools/lang_sync (built here for the build machine:
# no Python, nothing to install): it brings lang/*.po up to date with the
# sources' tr / trf / N_ strings, checks them, writes build/languages.txt
# (what is left to do and how) and languages_todo.txt, prints a one-line
# summary, and touches build/languages.stamp when a catalogue changed.  It
# never fails the build: an untranslated string shows in English.
#
# Each lang/<code>.po is built into an executable as a resource of the type
# GT3LANG named as the code upper case (lang/pt_BR.po: PT_BR); the resources
# depend on the files and on the stamp (a catalogue the sync changed during
# this build is built in at once).  The .po files are not the sync's
# declared outputs: a "clean" must never delete them.
if(NOT TARGET gt3_lang_sync)
    add_executable(gt3_lang_sync
        ${GT3PC_ROOT}/tools/lang_sync/lang_sync.cpp
        ${GT3PC_ROOT}/src/platform/overlay_font.cpp
        ${GT3PC_ROOT}/src/platform/ui_text.cpp)
    target_include_directories(gt3_lang_sync PRIVATE ${GT3PC_ROOT}/src)
    set_target_properties(gt3_lang_sync PROPERTIES OUTPUT_NAME lang_sync
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
    add_custom_target(gt3_lang_sync_run ALL
        COMMAND gt3_lang_sync --root ${GT3PC_ROOT} --build ${CMAKE_BINARY_DIR}
        BYPRODUCTS ${CMAKE_BINARY_DIR}/languages.stamp ${CMAKE_BINARY_DIR}/languages.txt
                   ${CMAKE_BINARY_DIR}/languages_todo.txt
        VERBATIM)
endif()

function(gt3pc_languages target)
    add_dependencies(${target} gt3_lang_sync_run)
    file(GLOB pos LIST_DIRECTORIES false CONFIGURE_DEPENDS ${GT3PC_ROOT}/lang/*.po)
    if(NOT pos)
        return()
    endif()
    set(rc "")
    foreach(po ${pos})
        get_filename_component(name ${po} NAME_WE)
        string(TOUPPER ${name} name)
        string(APPEND rc "${name} GT3LANG \"${po}\"\n")
    endforeach()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_languages.rc)
    file(CONFIGURE OUTPUT ${out} CONTENT "${rc}" @ONLY)
    target_sources(${target} PRIVATE ${out})
    set_property(SOURCE ${out} APPEND PROPERTY OBJECT_DEPENDS ${pos} ${CMAKE_BINARY_DIR}/languages.stamp)
endfunction()
