
function(target_add_auv3_standalone_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            BUNDLE_IDENTIFIER
            BUNDLE_VERSION

            AUV3_TARGET

            AU_TYPE
            AU_SUBTYPE
            AU_MANUFACTURER

            MACOS_ICON
            )
    cmake_parse_arguments(AUSA "" "${oneValueArgs}" "" ${ARGN})

    if (NOT APPLE)
        message(STATUS "clap-wrapper: auv3 standalone is only available on macOS")
        return()
    endif()

    if (NOT DEFINED AUSA_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_auv3_standalone_wrapper requires a TARGET")
    endif()

    if (NOT TARGET ${AUSA_TARGET})
        message(FATAL_ERROR "clap-wrapper: auv3-standalone target must be a target")
    endif()

    if (NOT DEFINED AUSA_AUV3_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_auv3_standalone_wrapper requires AUV3_TARGET")
    endif()

    if (NOT DEFINED AUSA_OUTPUT_NAME)
        set(AUSA_OUTPUT_NAME "${AUSA_TARGET}")
    endif()

    if (NOT DEFINED AUSA_BUNDLE_VERSION)
        set(AUSA_BUNDLE_VERSION "1.0")
    endif()

    if (NOT DEFINED AUSA_BUNDLE_IDENTIFIER)
        string(MAKE_C_IDENTIFIER ${AUSA_OUTPUT_NAME} outidentifier)
        string(REPLACE "_" "-" repout ${outidentifier})
        set(AUSA_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${repout}.auv3standalone")
    endif()

    if (NOT DEFINED AUSA_AU_TYPE)
        set(AUSA_AU_TYPE "aufx")
    endif()

    if (NOT DEFINED AUSA_AU_SUBTYPE)
        set(AUSA_AU_SUBTYPE "none")
    endif()

    if (NOT DEFINED AUSA_AU_MANUFACTURER)
        set(AUSA_AU_MANUFACTURER "none")
    endif()

    if (NOT DEFINED AUSA_MACOS_ICON)
        set(AUSA_MACOS_ICON "")
    endif()

    message(STATUS "clap-wrapper: Adding AUv3 Standalone to target ${AUSA_TARGET} for '${AUSA_OUTPUT_NAME}'")

    # --- XIB ---
    set(MAIN_XIB "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/auv3/MainMenu.xib")
    set(SA_OUTPUT_NAME "${AUSA_OUTPUT_NAME}")
    set(GEN_XIB "${CMAKE_BINARY_DIR}/generated_xib/${AUSA_TARGET}/MainMenu.xib")
    configure_file(${MAIN_XIB} ${GEN_XIB})

    # --- Sources ---
    target_sources(${AUSA_TARGET} PRIVATE
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasauv3standalone.mm"
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/auv3/AUv3HostAppDelegate.mm"
            ${GEN_XIB}
            )

    target_include_directories(${AUSA_TARGET} PRIVATE
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/auv3"
            )

    target_compile_options(${AUSA_TARGET} PRIVATE -fobjc-arc -fno-char8_t)

    # --- Suppress ObjC nullability warnings bleeding into C++ ---
    target_compile_options(${AUSA_TARGET} PRIVATE -Wno-nullability-completeness)

    # --- Frameworks (no RtAudio/RtMidi, no CLAP SDK) ---
    target_link_libraries(${AUSA_TARGET} PRIVATE
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework CoreMIDI"
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework AppKit"
            )

    # --- FourCC conversion: turn 4-char string into a uint32 compile definition ---
    # We pass the raw strings and do the conversion in the source code
    target_compile_definitions(${AUSA_TARGET} PRIVATE
            AU_TYPE_STR="${AUSA_AU_TYPE}"
            AU_SUBTYPE_STR="${AUSA_AU_SUBTYPE}"
            AU_MANUFACTURER_STR="${AUSA_AU_MANUFACTURER}"
            AUV3_STANDALONE_OUTPUT_NAME="${AUSA_OUTPUT_NAME}"
            )

    # --- App bundle properties ---
    set_target_properties(${AUSA_TARGET} PROPERTIES
            BUNDLE TRUE
            BUNDLE_NAME "${AUSA_OUTPUT_NAME}"
            BUNDLE_EXTENSION app
            OUTPUT_NAME "${AUSA_OUTPUT_NAME}"
            MACOSX_BUNDLE_BUNDLE_NAME "${AUSA_OUTPUT_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${AUSA_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/auv3/Info.plist.in"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${AUSA_BUNDLE_IDENTIFIER}"
            RESOURCE "${GEN_XIB}"
            )

    # --- XIB -> NIB for non-Xcode generators ---
    if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
        message(STATUS "clap-wrapper: ejecting xib->nib rules manually for ${CMAKE_GENERATOR} on ${AUSA_TARGET}")
        find_program(IBTOOL ibtool REQUIRED)
        add_custom_command(TARGET ${AUSA_TARGET} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo ${IBTOOL} --compile "$<TARGET_FILE_DIR:${AUSA_TARGET}>/../Resources/MainMenu.nib" ${GEN_XIB}
                COMMAND ${IBTOOL} --compile "$<TARGET_FILE_DIR:${AUSA_TARGET}>/../Resources/MainMenu.nib" ${GEN_XIB}
                )
    endif()

    # --- Icon ---
    if(NOT "${AUSA_MACOS_ICON}" STREQUAL "")
        add_custom_command(TARGET ${AUSA_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${AUSA_MACOS_ICON} "$<TARGET_FILE_DIR:${AUSA_TARGET}>/../Resources/Icon.icns"
        )
    endif()

    # --- Embed the .appex in Contents/PlugIns/ ---
    add_dependencies(${AUSA_TARGET} ${AUSA_AUV3_TARGET})

    # Get the appex bundle path. For MODULE libraries built as bundles, the output
    # is at LIBRARY_OUTPUT_DIRECTORY or the default build dir.
    # The appex bundle is a directory at: <TARGET_FILE_DIR>/../../..
    # which is the same as the .appex top-level directory.
    # We use TARGET_BUNDLE_DIR for BUNDLE targets to get the correct path.
    set(_auv3_appex_src "$<TARGET_BUNDLE_DIR:${AUSA_AUV3_TARGET}>")
    set(_auv3_appex_dst "$<TARGET_BUNDLE_DIR:${AUSA_TARGET}>/Contents/PlugIns/$<TARGET_PROPERTY:${AUSA_AUV3_TARGET},OUTPUT_NAME>.appex")

    add_custom_command(TARGET ${AUSA_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:${AUSA_TARGET}>/Contents/PlugIns"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_auv3_appex_src}"
                "${_auv3_appex_dst}"
            )

    # No extra signing step: the copied appex keeps the entitlement-bearing
    # signature wrap_auv3.cmake applied. (AUv3 requires the Xcode generator,
    # so a non-Xcode re-sign branch here would be unreachable — and a plain
    # `codesign -s -` without --entitlements would strip the app-sandbox
    # entitlement macOS needs to register the appex.)

endfunction(target_add_auv3_standalone_wrapper)
