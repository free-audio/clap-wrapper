
function(target_add_auv2_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            BUNDLE_IDENTIFIER
            BUNDLE_VERSION
            RESOURCE_DIRECTORY

            MANUFACTURER_NAME
            MANUFACTURER_CODE
            SUBTYPE_CODE
            INSTRUMENT_TYPE

            CLAP_TARGET_FOR_CONFIG

            # AUV2 uses a CFDictionary to store state so
            # we need to choose what keys to populate it with.
            # The wrapper by default uses the key choices from
            # the AUSDK example, but for compatability with people
            # moving from other frameworks we do support other schema.
            #
            # SUpported schema today are
            # - WRAPPER (the default)
            # - JUCE (use the JUCE AUV2 default)
            DICTIONARY_STREAM_FORMAT

            # These two are aliases of each other but the one without the X
            # came first and doesn't match the rest of CMake naming standards
            MACOS_EMBEDDED_CLAP_LOCATION
            MACOSX_EMBEDDED_CLAP_LOCATION
            )
    cmake_parse_arguments(AUV2 "" "${oneValueArgs}" "" ${ARGN})


    if (NOT DEFINED AUV2_MACOS_EMBEDDED_CLAP_LOCATION AND DEFINED AUV2_MACOSX_EMBEDDED_CLAP_LOCATION)
        # resolve the alias
        set(AUV2_MACOS_EMBEDDED_CLAP_LOCATION ${AUV2_MACOSX_EMBEDDED_CLAP_LOCATION})
    endif()

    if (NOT DEFINED AUV2_MACOSX_EMBEDDED_CLAP_LOCATION AND DEFINED AUV2_MACOS_EMBEDDED_CLAP_LOCATION)
        # resolve the alias
        set(AUV2_MACOSX_EMBEDDED_CLAP_LOCATION ${AUV2_MACOS_EMBEDDED_CLAP_LOCATION})
    endif()

    if (NOT APPLE)
        message(STATUS "clap-wrapper: auv2 is only available on macOS")
        return()
    endif()

    guarantee_auv2sdk()

    if (NOT DEFINED AUV2_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_auv2_wrapper requires a target")
    endif ()

    if (NOT TARGET ${AUV2_TARGET})
        message(FATAL_ERROR "clap-wrapper: auv2-target must be a target")
    endif ()

    if (NOT DEFINED AUV2_BUNDLE_VERSION)
        message(WARNING "clap-wrapper: bundle version not defined. Chosing ${PROJECT_VERSION}")
        set(AUV2_BUNDLE_VERSION ${PROJECT_VERSION})
    endif ()

    if (NOT DEFINED AUV2_RESOURCE_DIRECTORY)
        set(AUV2_RESOURCE_DIRECTORY "")
    endif()

    # We need a build helper which ejects our config code for info-plist and entry points
    set(bhtg ${AUV2_TARGET}-build-helper)
    set(bhsc "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/build-helper/")
    add_executable(${bhtg} ${bhsc}/build-helper.cpp)
    target_link_libraries(${bhtg} PRIVATE
            clap-wrapper-compile-options
            clap-wrapper-shared-detail
            macos_filesystem_support
            "-framework Foundation"
            "-framework CoreFoundation"
            )
    set(bhtgoutdir "${CMAKE_CURRENT_BINARY_DIR}/${AUV2_TARGET}-build-helper-output")

    add_custom_command(TARGET ${bhtg} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "clap-wrapper: auv2 configuration output dir is ${bhtgoutdir}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${bhtgoutdir}"
            )

    add_dependencies(${AUV2_TARGET} ${bhtg})

    if (DEFINED AUV2_CLAP_TARGET_FOR_CONFIG)
        set(clpt ${AUV2_CLAP_TARGET_FOR_CONFIG})
        message(STATUS "clap-wrapper: building auv2 based on target ${AUV2_CLAP_TARGET_FOR_CONFIG}")
        get_property(ton TARGET ${clpt} PROPERTY LIBRARY_OUTPUT_NAME)
        set(AUV2_OUTPUT_NAME "${ton}")
        set(AUV2_SUBTYPE_CODE "Fooo")
        set(AUV2_INSTRUMENT_TYPE "aumu")

        add_dependencies(${AUV2_TARGET} ${clpt})
        add_dependencies(${bhtg} ${clpt})

        add_custom_command(
                TARGET ${bhtg}
                POST_BUILD
                WORKING_DIRECTORY ${bhtgoutdir}
                BYPRODUCTS ${bhtgoutdir}/auv2_Info.plist ${bhtgoutdir}/generated_entrypoints.hxx
                COMMAND codesign -s - -f "$<TARGET_FILE:${bhtg}>"
                COMMAND $<TARGET_FILE:${bhtg}> --fromclap
                "${AUV2_OUTPUT_NAME}"
                "$<TARGET_FILE:${clpt}>" "${AUV2_BUNDLE_VERSION}"
                "${AUV2_MANUFACTURER_CODE}" "${AUV2_MANUFACTURER_NAME}"
        )
    elseif (DEFINED AUV2_MACOSX_EMBEDDED_CLAP_LOCATION)
        message(STATUS "clap-wrapper: building auv2 based on clap ${AUV2_MACOSX_EMBEDDED_CLAP_LOCATION}")
        set(AUV2_SUBTYPE_CODE "----")
        set(AUV2_INSTRUMENT_TYPE "aumu")

        add_custom_command(
                TARGET ${bhtg}
                POST_BUILD
                WORKING_DIRECTORY ${bhtgoutdir}
                BYPRODUCTS ${bhtgoutdir}/auv2_Info.plist ${bhtgoutdir}/generated_entrypoints.hxx
                COMMAND codesign -s - -f "$<TARGET_FILE:${bhtg}>"
                COMMAND $<TARGET_FILE:${bhtg}> --fromclap
                "${AUV2_OUTPUT_NAME}"
                "${AUV2_MACOSX_EMBEDDED_CLAP_LOCATION}" "${AUV2_BUNDLE_VERSION}"
                "${AUV2_MANUFACTURER_CODE}" "${AUV2_MANUFACTURER_NAME}"
        )
    else ()
        if (NOT DEFINED AUV2_OUTPUT_NAME)
            message(FATAL_ERROR "clap-wrapper: target_add_auv2_wrapper requires an output name")
        endif ()

        if (NOT DEFINED AUV2_SUBTYPE_CODE)
            message(FATAL_ERROR "clap-wrapper: For nontarget build specify AUV2 subtype code (4 chars)")
        endif ()

        if (NOT DEFINED AUV2_MANUFACTURER_NAME)
            message(FATAL_ERROR "clap-wrapper: For nontarget build specify AUV2 manufacturer name")
        endif ()

        if (NOT DEFINED AUV2_MANUFACTURER_CODE)
            message(FATAL_ERROR "clap-wrapper: For nontarget build specify AUV2 manufacturer code (4 chars)")
        endif ()

        if (NOT DEFINED AUV2_INSTRUMENT_TYPE)
            message(WARNING "clap-wrapper: auv2 instrument type not specified. Using aumu")
            set(AUV2_INSTRUMENT_TYPE "aumu")
        endif ()

        add_custom_command(
                TARGET ${bhtg}
                POST_BUILD
                WORKING_DIRECTORY ${bhtgoutdir}
                BYPRODUCTS ${bhtgoutdir}/auv2_Info.plist ${bhtgoutdir}/generated_entrypoints.hxx
                COMMAND codesign -s - -f "$<TARGET_FILE:${bhtg}>"
                COMMAND $<TARGET_FILE:${bhtg}> --explicit
                "${AUV2_OUTPUT_NAME}" "${AUV2_BUNDLE_VERSION}"
                "${AUV2_INSTRUMENT_TYPE}" "${AUV2_SUBTYPE_CODE}"
                "${AUV2_MANUFACTURER_CODE}" "${AUV2_MANUFACTURER_NAME}"
        )
    endif ()

    string(MAKE_C_IDENTIFIER ${AUV2_OUTPUT_NAME} outidentifier)

    if ("${AUV2_BUNDLE_IDENTIFIER}" STREQUAL "")
        string(REPLACE "_" "-" repout ${outidentifier})
        set(AUV2_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${repout}.auv2")
    endif ()

    set(AUV2_MANUFACTURER_NAME ${AUV2_MANUFACTURER_NAME} PARENT_SCOPE)
    set(AUV2_MANUFACTURER_CODE ${AUV2_MANUFACTURER_CODE} PARENT_SCOPE)
    configure_file(${bhsc}/auv2_infoplist_top.in
            ${bhtgoutdir}/auv2_infoplist_top)

    set(AUV2_INSTRUMENT_TYPE ${AUV2_INSTRUMENT_TYPE} PARENT_SCOPE)
    set(AUV2_SUBTYPE_CODE ${AUV2_SUBTYPE_CODE} PARENT_SCOPE)

    message(STATUS "clap-wrapper: Adding AUV2 Wrapper to target ${AUV2_TARGET} generating '${AUV2_OUTPUT_NAME}.component'")

    target_sources(${AUV2_TARGET} PRIVATE ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/os/macos.mm)

    # This is a placeholder dummy until we actually write the AUv2
    # Similarly the subordinate library being an interface below
    # is a placeholder. When we write it we will follow a similar
    # split trick as for the vst3, mostly (but AUV2 is a bit different
    # with info.plist and entrypoint-per-instance stuff)
    target_sources(${AUV2_TARGET} PRIVATE
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasauv2.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/auv2_shared.h
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/auv2_shared.mm
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/auv2_base_classes.h
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/process.h
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/process.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/wrappedview.mm
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/parameter.h
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv2/parameter.cpp
            
	    ${bhtgoutdir}/generated_entrypoints.hxx)
    target_compile_options(${AUV2_TARGET} PRIVATE -fno-char8_t)
    if (DEFINED AUV2_DICTIONARY_STREAM_FORMAT)
        # valud options check
        set (sf ${AUV2_DICTIONARY_STREAM_FORMAT})
        if (NOT ((${sf} STREQUAL "JUCE") OR (${sf} STREQUAL "WRAPPER")))
            message(FATAL_ERROR "Unrecognized DICTIONARY_STREAM_FORMAT in AUV2: ${sf}")
        endif()
        target_compile_definitions(${AUV2_TARGET} PRIVATE DICTIONARY_STREAM_FORMAT_${AUV2_DICTIONARY_STREAM_FORMAT}=1)
    endif()

    if (NOT TARGET ${AUV2_TARGET}-clap-wrapper-auv2-lib)
        # For now make this an interface
        add_library(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE)
        target_include_directories(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE "${bhtgoutdir}" "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE clap base-sdk-auv2)

        # clap-wrapper-extensions are PUBLIC, so a clap linking the library can access the clap-wrapper-extensions
        target_link_libraries(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE clap-wrapper-extensions clap-wrapper-shared-detail clap-wrapper-compile-options)

    endif ()

    set_target_properties(${AUV2_TARGET} PROPERTIES LIBRARY_OUTPUT_NAME "${AUV2_OUTPUT_NAME}")
    target_link_libraries(${AUV2_TARGET} PUBLIC ${AUV2_TARGET}-clap-wrapper-auv2-lib)

    if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
        set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
    endif ()

    target_link_libraries(${AUV2_TARGET} PUBLIC
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework AppKit"
            "-framework AudioToolbox"
            "-framework CoreMIDI")

    set_target_properties(${AUV2_TARGET} PROPERTIES
            BUNDLE True
            BUNDLE_EXTENSION component
            LIBRARY_OUTPUT_NAME ${AUV2_OUTPUT_NAME}
            MACOSX_BUNDLE_GUI_IDENTIFIER "${AUV2_BUNDLE_IDENTIFIER}.component"
            MACOSX_BUNDLE_BUNDLE_NAME ${AUV2_OUTPUT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${AUV2_BUNDLE_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${AUV2_BUNDLE_VERSION}
            )

    # This is "PRE_BUILD" because the target is created at cmake time and we want to beat xcode signing in order
    # it is *not* a MACOSX_BUNDLE_INFO_PLIST since that is a configure not build time concept so doesn't work
    # with compile time generated files
    add_custom_command(TARGET ${AUV2_TARGET} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${bhtgoutdir}/auv2_Info.plist $<TARGET_FILE_DIR:${AUV2_TARGET}>/../Info.plist)

    # XCode needs a special extra flag
    set_target_properties(${AUV2_TARGET} PROPERTIES XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${AUV2_BUNDLE_IDENTIFIER}.component")

    macos_include_clap_in_bundle(TARGET ${AUV2_TARGET}
            MACOS_EMBEDDED_CLAP_LOCATION ${AUV2_MACOSX_EMBEDDED_CLAP_LOCATION})
    macos_bundle_flag(TARGET ${AUV2_TARGET})

    # Copy resource directory, if defined
    if(NOT AUV2_RESOURCE_DIRECTORY STREQUAL "")
        message(WARNING "RESOURCE_DIRECTORY defined, but not (yet) supported for AUV2")
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${AUV2_TARGET} FLAVOR auv2)
    endif ()
endfunction(target_add_auv2_wrapper)
