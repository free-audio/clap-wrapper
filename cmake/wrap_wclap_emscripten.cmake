# This adds a WCLAP target, but must only be called when using Emscripten's toolchain.

function(target_add_wclap_configuration)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            RESOURCE_DIRECTORY
    )
    cmake_parse_arguments(TCLP "" "${oneValueArgs}" "" ${ARGN} )

    if (NOT EMSCRIPTEN)
        message(FATAL_ERROR "Do not call this outside the Emscripten toolchain")
    endif()

    if (NOT DEFINED TCLP_TARGET)
        message(FATAL_ERROR "You must define TARGET in target_library_is_clap")
    endif()

    if (NOT DEFINED TCLP_OUTPUT_NAME)
        message(STATUS "Using target name as clap name in target_library_is_clap")
        set(TCLP_OUTPUT_NAME TCLP_TARGET)
    endif()

    set_target_properties(${TCLP_TARGET}
            PROPERTIES
            OUTPUT_NAME "${TCLP_OUTPUT_NAME}"
            SUFFIX ".wclap/module.wasm"
            PREFIX ""
    )
    # Make sure directory exists
    add_custom_command(TARGET ${TCLP_TARGET} PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TCLP_TARGET}>"
    )

    # Copy resource directory, if defined
    if(NOT TCLP_RESOURCE_DIRECTORY STREQUAL "")
        add_custom_command(TARGET ${TCLP_TARGET} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${TCLP_RESOURCE_DIRECTORY}" "$<TARGET_FILE_DIR:${TCLP_TARGET}>"
        )
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${TCLP_TARGET} FLAVOR wclap)
    endif()
endfunction(target_add_wclap_configuration)
