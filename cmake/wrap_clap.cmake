# You may wonder why we have a 'wrap_clap.cmake' when the clap wrapper is
# starting with a CLAP. Well, quite a few things to go from a cmake MODULE
# to a clap are required, including copy after build logic, setting up the
# dll, and so forth.
#
# Rather than copy and paste this code around, we provide a convenience function
# for you symmetric with the other wrapper functions


function(target_add_clap_configuration)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            BUNDLE_IDENTIFIER
            BUNDLE_VERSION
            RESOURCE_DIRECTORY
    )
    cmake_parse_arguments(TCLP "" "${oneValueArgs}" "" ${ARGN} )

    if (NOT DEFINED TCLP_TARGET)
        message(FATAL_ERROR "You must define TARGET in target_library_is_clap")
    endif()

    if (NOT DEFINED TCLP_OUTPUT_NAME)
        message(STATUS "Using target name as clap name in target_libarry_is_clap")
        set(TCLP_OUTPUT_NAME TCLP_TARGET)
    endif()

    if (NOT DEFINED TCLP_BUNDLE_IDENTIFIER)
        message(STATUS "Using sst default macos bundle prefix in target_libarry_is_clap")
        set(TCLP_BUNDLE_IDENTIFIER "clap.${TCLP_TARGET}")
    endif()

    if (NOT DEFINED TCLP_BUNDLE_VERSION)
        message(STATUS "No bundle version in target_library_is_clap; using 0.1")
        set(TCLP_BUNDLE_VERSION "0.1")
    endif()

    if (NOT DEFINED TCLP_RESOURCE_DIRECTORY)
        set(TCLP_RESOURCE_DIRECTORY "")
    endif()

    if(APPLE)
        set_target_properties(${TCLP_TARGET} PROPERTIES
                BUNDLE True
                BUNDLE_EXTENSION clap
                LIBRARY_OUTPUT_NAME ${TCLP_OUTPUT_NAME}
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_GUI_IDENTIFIER ${TCLP_BUNDLE_IDENTIFIER}
                MACOSX_BUNDLE_BUNDLE_NAME ${TCLP_OUTPUT_NAME}
                MACOSX_BUNDLE_BUNDLE_VERSION "${TCLP_BUNDLE_VERSION}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${TCLP_BUNDLE_VERSION}"
        )

        # Copy resource directory, if defined
        if(NOT TCLP_RESOURCE_DIRECTORY STREQUAL "")
            add_custom_command(TARGET ${TCLP_TARGET} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "${TCLP_RESOURCE_DIRECTORY}" "$<TARGET_FILE_DIR:${TCLP_TARGET}>/../Resources"
            )
        endif()

        macos_bundle_flag(TARGET ${TCLP_TARGET})

        set_target_properties(${TCLP_TARGET}
                PROPERTIES
                MACOSX_BUNDLE TRUE
                XCODE_ATTRIBUTE_GENERATE_PKGINFO_FILE "YES"
        )
    elseif(UNIX)
        set_target_properties(${TCLP_TARGET}
                PROPERTIES
                OUTPUT_NAME ${TCLP_OUTPUT_NAME}
                SUFFIX ".clap"
                PREFIX "")
    else()
        set_target_properties(${TCLP_TARGET}
                PROPERTIES
                OUTPUT_NAME ${TCLP_OUTPUT_NAME}
                SUFFIX ".clap"
                PREFIX ""
                LIBRARY_OUTPUT_DIRECTORY CLAP
        )
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${TCLP_TARGET} FLAVOR clap)
    endif()
endfunction(target_add_clap_configuration)
