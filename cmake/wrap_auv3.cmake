
function(target_add_auv3_wrapper)
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

            MACOS_EMBEDDED_CLAP_LOCATION
            MACOSX_EMBEDDED_CLAP_LOCATION
            )
    cmake_parse_arguments(AUV3 "" "${oneValueArgs}" "" ${ARGN})

    if (NOT DEFINED AUV3_MACOS_EMBEDDED_CLAP_LOCATION AND DEFINED AUV3_MACOSX_EMBEDDED_CLAP_LOCATION)
        set(AUV3_MACOS_EMBEDDED_CLAP_LOCATION ${AUV3_MACOSX_EMBEDDED_CLAP_LOCATION})
    endif()

    if (NOT DEFINED AUV3_MACOSX_EMBEDDED_CLAP_LOCATION AND DEFINED AUV3_MACOS_EMBEDDED_CLAP_LOCATION)
        set(AUV3_MACOSX_EMBEDDED_CLAP_LOCATION ${AUV3_MACOS_EMBEDDED_CLAP_LOCATION})
    endif()

    if (NOT APPLE)
        message(STATUS "clap-wrapper: auv3 is only available on macOS/iOS")
        return()
    endif()

    # AUv3 does NOT require the AudioUnit SDK (ausdk) - it uses AudioToolbox.framework directly

    if (NOT DEFINED AUV3_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_auv3_wrapper requires a target")
    endif ()

    if (NOT TARGET ${AUV3_TARGET})
        message(FATAL_ERROR "clap-wrapper: auv3-target must be a target")
    endif ()

    if (NOT DEFINED AUV3_BUNDLE_VERSION)
        message(WARNING "clap-wrapper: bundle version not defined. Choosing ${PROJECT_VERSION}")
        set(AUV3_BUNDLE_VERSION ${PROJECT_VERSION})
    endif ()

    if (NOT DEFINED AUV3_RESOURCE_DIRECTORY)
        set(AUV3_RESOURCE_DIRECTORY "")
    endif()

    # Build helper to generate Info.plist and entry points
    set(bhtg ${AUV3_TARGET}-auv3-build-helper)
    set(bhsc "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/build-helper/")
    add_executable(${bhtg} ${bhsc}/build-helper.cpp)
    target_link_libraries(${bhtg} PRIVATE
            clap-wrapper-compile-options
            clap-wrapper-shared-detail
            macos_filesystem_support
            "-framework Foundation"
            "-framework CoreFoundation"
            )
    set(bhtgoutdir "${CMAKE_CURRENT_BINARY_DIR}/${AUV3_TARGET}-auv3-build-helper-output")

    add_custom_command(TARGET ${bhtg} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "clap-wrapper: auv3 configuration output dir is ${bhtgoutdir}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${bhtgoutdir}"
            )

    add_dependencies(${AUV3_TARGET} ${bhtg})

    if (DEFINED AUV3_CLAP_TARGET_FOR_CONFIG)
        set(clpt ${AUV3_CLAP_TARGET_FOR_CONFIG})
        message(STATUS "clap-wrapper: building auv3 based on target ${AUV3_CLAP_TARGET_FOR_CONFIG}")
        get_property(ton TARGET ${clpt} PROPERTY LIBRARY_OUTPUT_NAME)
        set(AUV3_OUTPUT_NAME "${ton}")

        if (NOT DEFINED AUV3_MANUFACTURER_CODE)
            set(AUV3_MANUFACTURER_CODE "errr")
        endif()
        if (NOT DEFINED AUV3_MANUFACTURER_NAME)
            set(AUV3_MANUFACTURER_NAME "errr")
        endif()
        if (NOT DEFINED AUV3_SUBTYPE_CODE)
            set(AUV3_SUBTYPE_CODE "errr")
        endif()
        if (NOT DEFINED AUV3_INSTRUMENT_TYPE)
            set(AUV3_INSTRUMENT_TYPE "errr")
        endif()

        add_dependencies(${AUV3_TARGET} ${clpt})
        add_dependencies(${bhtg} ${clpt})

        add_custom_command(
                TARGET ${bhtg}
                POST_BUILD
                WORKING_DIRECTORY ${bhtgoutdir}
                BYPRODUCTS ${bhtgoutdir}/auv3_Info.plist ${bhtgoutdir}/generated_auv3_entrypoints.hxx
                COMMAND codesign -s - -f "$<TARGET_FILE:${bhtg}>"
                COMMAND $<TARGET_FILE:${bhtg}> --fromclap
                "${AUV3_OUTPUT_NAME}"
                "$<TARGET_FILE:${clpt}>" "${AUV3_BUNDLE_VERSION}"
                "${AUV3_MANUFACTURER_CODE}" "${AUV3_MANUFACTURER_NAME}"
                "${AUV3_INSTRUMENT_TYPE}" "${AUV3_SUBTYPE_CODE}"
        )
    elseif (DEFINED AUV3_MACOSX_EMBEDDED_CLAP_LOCATION)
        message(STATUS "clap-wrapper: building auv3 based on clap ${AUV3_MACOSX_EMBEDDED_CLAP_LOCATION}")

        if (NOT DEFINED AUV3_MANUFACTURER_CODE)
            set(AUV3_MANUFACTURER_CODE "errr")
        endif()
        if (NOT DEFINED AUV3_MANUFACTURER_NAME)
            set(AUV3_MANUFACTURER_NAME "errr")
        endif()
        if (NOT DEFINED AUV3_SUBTYPE_CODE)
            set(AUV3_SUBTYPE_CODE "errr")
        endif()
        if (NOT DEFINED AUV3_INSTRUMENT_TYPE)
            set(AUV3_INSTRUMENT_TYPE "errr")
        endif()

        add_custom_command(
                TARGET ${bhtg}
                POST_BUILD
                WORKING_DIRECTORY ${bhtgoutdir}
                BYPRODUCTS ${bhtgoutdir}/auv3_Info.plist ${bhtgoutdir}/generated_auv3_entrypoints.hxx
                COMMAND codesign -s - -f "$<TARGET_FILE:${bhtg}>"
                COMMAND $<TARGET_FILE:${bhtg}> --fromclap
                "${AUV3_OUTPUT_NAME}"
                "${AUV3_MACOSX_EMBEDDED_CLAP_LOCATION}" "${AUV3_BUNDLE_VERSION}"
                "${AUV3_MANUFACTURER_CODE}" "${AUV3_MANUFACTURER_NAME}"
                "${AUV3_INSTRUMENT_TYPE}" "${AUV3_SUBTYPE_CODE}"
        )
    else ()
        message(STATUS "clap-wrapper: using cmake configuration for auv3")
        if (NOT DEFINED AUV3_OUTPUT_NAME)
            message(FATAL_ERROR "clap-wrapper: target_add_auv3_wrapper requires an output name")
        endif ()

        if (NOT DEFINED AUV3_SUBTYPE_CODE)
            message(FATAL_ERROR "clap-wrapper: For nontarget build specify AUV3 subtype code (4 chars)")
        endif ()

        if (NOT DEFINED AUV3_MANUFACTURER_NAME)
            message(FATAL_ERROR "clap-wrapper: For nontarget build specify AUV3 manufacturer name")
        endif ()

        if (NOT DEFINED AUV3_MANUFACTURER_CODE)
            message(FATAL_ERROR "clap-wrapper: For nontarget build specify AUV3 manufacturer code (4 chars)")
        endif ()

        if (NOT DEFINED AUV3_INSTRUMENT_TYPE)
            message(WARNING "clap-wrapper: auv3 instrument type not specified. Using aumu")
            set(AUV3_INSTRUMENT_TYPE "aumu")
        endif ()

        add_custom_command(
                TARGET ${bhtg}
                POST_BUILD
                WORKING_DIRECTORY ${bhtgoutdir}
                BYPRODUCTS ${bhtgoutdir}/auv3_Info.plist ${bhtgoutdir}/generated_auv3_entrypoints.hxx
                COMMAND codesign -s - -f "$<TARGET_FILE:${bhtg}>"
                COMMAND $<TARGET_FILE:${bhtg}> --explicit
                "${AUV3_OUTPUT_NAME}" "${AUV3_BUNDLE_VERSION}"
                "${AUV3_INSTRUMENT_TYPE}" "${AUV3_SUBTYPE_CODE}"
                "${AUV3_MANUFACTURER_CODE}" "${AUV3_MANUFACTURER_NAME}"
        )
    endif ()

    string(MAKE_C_IDENTIFIER ${AUV3_OUTPUT_NAME} outidentifier)

    if ("${AUV3_BUNDLE_IDENTIFIER}" STREQUAL "")
        string(REPLACE "_" "-" repout ${outidentifier})
        set(AUV3_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${repout}.auv3")
    endif ()

    set(AUV3_MANUFACTURER_NAME ${AUV3_MANUFACTURER_NAME} PARENT_SCOPE)
    set(AUV3_MANUFACTURER_CODE ${AUV3_MANUFACTURER_CODE} PARENT_SCOPE)
    configure_file(${bhsc}/auv3_infoplist_top.in
            ${bhtgoutdir}/auv3_infoplist_top)

    set(AUV3_INSTRUMENT_TYPE ${AUV3_INSTRUMENT_TYPE} PARENT_SCOPE)
    set(AUV3_SUBTYPE_CODE ${AUV3_SUBTYPE_CODE} PARENT_SCOPE)

    message(STATUS "clap-wrapper: Adding AUv3 Wrapper to target ${AUV3_TARGET} generating '${AUV3_OUTPUT_NAME}.appex'")

    target_sources(${AUV3_TARGET} PRIVATE ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/os/macos.mm)

    target_sources(${AUV3_TARGET} PRIVATE
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasauv3.mm
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/auv3_audiounit.mm
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/auv3_parameters.mm
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/process.mm
            ${bhtgoutdir}/generated_auv3_entrypoints.hxx)
    target_compile_options(${AUV3_TARGET} PRIVATE -fno-char8_t -fobjc-arc)

    if (NOT TARGET ${AUV3_TARGET}-clap-wrapper-auv3-lib)
        add_library(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE)
        target_include_directories(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE "${bhtgoutdir}" "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE clap)
        target_link_libraries(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE clap-wrapper-extensions clap-wrapper-shared-detail clap-wrapper-compile-options)
    endif ()

    set_target_properties(${AUV3_TARGET} PROPERTIES LIBRARY_OUTPUT_NAME "${AUV3_OUTPUT_NAME}")
    target_link_libraries(${AUV3_TARGET} PUBLIC ${AUV3_TARGET}-clap-wrapper-auv3-lib)

    if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
        set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
    endif ()

    target_link_libraries(${AUV3_TARGET} PUBLIC
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework AppKit"
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework CoreAudio"
            "-framework CoreAudioKit"
            "-framework CoreMIDI")

    set_target_properties(${AUV3_TARGET} PROPERTIES
            BUNDLE True
            BUNDLE_EXTENSION appex
            LIBRARY_OUTPUT_NAME ${AUV3_OUTPUT_NAME}
            MACOSX_BUNDLE_GUI_IDENTIFIER "${AUV3_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_NAME ${AUV3_OUTPUT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${AUV3_BUNDLE_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${AUV3_BUNDLE_VERSION}
            )

    # For Xcode: tell it to use the build-helper's plist as INFOPLIST_FILE so
    # Xcode's own "Process Info.plist" phase preserves our NSExtension block.
    # For non-Xcode generators: POST_BUILD copy works because there's no
    # implicit plist processing after POST_BUILD.
    if (CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(${AUV3_TARGET} PROPERTIES
                MACOSX_BUNDLE_INFO_PLIST "${bhtgoutdir}/auv3_Info.plist"
                )
    else()
        add_custom_command(TARGET ${AUV3_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${bhtgoutdir}/auv3_Info.plist $<TARGET_FILE_DIR:${AUV3_TARGET}>/../Info.plist
            COMMENT "Replacing Info.plist with build-helper generated version (contains NSExtension)")
    endif()

    set_target_properties(${AUV3_TARGET} PROPERTIES XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${AUV3_BUNDLE_IDENTIFIER}")

    macos_include_clap_in_bundle(TARGET ${AUV3_TARGET}
            MACOS_EMBEDDED_CLAP_LOCATION ${AUV3_MACOSX_EMBEDDED_CLAP_LOCATION})
    macos_bundle_flag(TARGET ${AUV3_TARGET})

    if(NOT AUV3_RESOURCE_DIRECTORY STREQUAL "")
        message(WARNING "RESOURCE_DIRECTORY defined, but not (yet) supported for AUV3")
    endif()

    # Set entitlements for sandboxing (required for AUv3 registration)
    set(AUV3_ENTITLEMENTS "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/auv3.entitlements")

    # For Xcode, set the entitlements via build settings
    set_target_properties(${AUV3_TARGET} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${AUV3_ENTITLEMENTS}"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
            XCODE_ATTRIBUTE_ENABLE_APP_SANDBOX "YES"
            )

    # Ad-hoc sign the appex with entitlements so macOS will register it as an Audio Unit
    add_custom_command(TARGET ${AUV3_TARGET} POST_BUILD
            COMMAND codesign -s - -f --entitlements "${AUV3_ENTITLEMENTS}" "$<TARGET_BUNDLE_DIR:${AUV3_TARGET}>"
            COMMENT "Ad-hoc signing AUv3 appex with sandbox entitlements"
            )

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${AUV3_TARGET} FLAVOR auv3)
    endif ()
endfunction(target_add_auv3_wrapper)
