# target_add_auv3_standalone_ios_wrapper — iOS counterpart to
# target_add_auv3_standalone_wrapper (macOS).
#
# Produces an iOS .app bundle that embeds the named AUv3 .appex in
# PlugIns/ and, at launch, instantiates the AU by AudioComponentDescription
# and presents the plugin's UIViewController as the root VC.
#
# Intended for development/testing — not App-Store-ready.

function(target_add_auv3_standalone_ios_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            BUNDLE_IDENTIFIER
            BUNDLE_VERSION

            AUV3_TARGET

            AU_TYPE
            AU_SUBTYPE
            AU_MANUFACTURER
            )
    cmake_parse_arguments(AUSAIOS "" "${oneValueArgs}" "" ${ARGN})

    if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(STATUS "clap-wrapper: auv3 iOS standalone is iOS-only — skipping on ${CMAKE_SYSTEM_NAME}")
        return()
    endif()

    if (NOT DEFINED AUSAIOS_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_auv3_standalone_ios_wrapper requires TARGET")
    endif()
    if (NOT TARGET ${AUSAIOS_TARGET})
        message(FATAL_ERROR "clap-wrapper: auv3-standalone-ios target must be a target")
    endif()
    if (NOT DEFINED AUSAIOS_AUV3_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_auv3_standalone_ios_wrapper requires AUV3_TARGET")
    endif()

    if (NOT DEFINED AUSAIOS_OUTPUT_NAME)
        set(AUSAIOS_OUTPUT_NAME "${AUSAIOS_TARGET}")
    endif()
    if (NOT DEFINED AUSAIOS_BUNDLE_VERSION)
        set(AUSAIOS_BUNDLE_VERSION "1.0")
    endif()
    if (NOT DEFINED AUSAIOS_BUNDLE_IDENTIFIER)
        string(MAKE_C_IDENTIFIER ${AUSAIOS_OUTPUT_NAME} outidentifier)
        string(REPLACE "_" "-" repout ${outidentifier})
        set(AUSAIOS_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${repout}.auv3standaloneios")
    endif()
    if (NOT DEFINED AUSAIOS_AU_TYPE)
        set(AUSAIOS_AU_TYPE "aufx")
    endif()
    if (NOT DEFINED AUSAIOS_AU_SUBTYPE)
        set(AUSAIOS_AU_SUBTYPE "none")
    endif()
    if (NOT DEFINED AUSAIOS_AU_MANUFACTURER)
        set(AUSAIOS_AU_MANUFACTURER "none")
    endif()

    if (DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
        set(AUSAIOS_IOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(AUSAIOS_IOS_DEPLOYMENT_TARGET "15.0")
    endif()

    message(STATUS "clap-wrapper: Adding AUv3 iOS Standalone to target ${AUSAIOS_TARGET} for '${AUSAIOS_OUTPUT_NAME}'")

    # --- Info.plist ---
    set(_plistoutdir "${CMAKE_CURRENT_BINARY_DIR}/${AUSAIOS_TARGET}-plist")
    file(MAKE_DIRECTORY "${_plistoutdir}")
    configure_file(
        "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/ios/auv3/Info.plist.in"
        "${_plistoutdir}/Info.plist"
        @ONLY)

    # --- Sources ---
    target_sources(${AUSAIOS_TARGET} PRIVATE
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/ios/auv3/ios_host_main.m"
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/ios/auv3/IOSHostAppDelegate.mm"
            )
    target_include_directories(${AUSAIOS_TARGET} PRIVATE
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/ios/auv3"
            )
    target_compile_options(${AUSAIOS_TARGET} PRIVATE -fobjc-arc)

    # AU identity is passed as 4-char string macros; the host converts to
    # FourCC at runtime (avoids awkward integer-literal packing in CMake).
    target_compile_definitions(${AUSAIOS_TARGET} PRIVATE
            AU_TYPE_STR="${AUSAIOS_AU_TYPE}"
            AU_SUBTYPE_STR="${AUSAIOS_AU_SUBTYPE}"
            AU_MANUFACTURER_STR="${AUSAIOS_AU_MANUFACTURER}"
            )

    target_link_libraries(${AUSAIOS_TARGET} PRIVATE
            "-framework Foundation"
            "-framework UIKit"
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework CoreAudioKit"
            "-framework CoreMIDI"
            )

    # --- Bundle properties ---
    # TARGETED_DEVICE_FAMILY "1,2" = iPhone + iPad. Must match the embedded
    # appex's setting; if the host is iPhone-only, pluginkit won't register
    # its AUv3 extension when the containing app runs on iPad.
    set_target_properties(${AUSAIOS_TARGET} PROPERTIES
            MACOSX_BUNDLE TRUE
            BUNDLE_NAME "${AUSAIOS_OUTPUT_NAME}"
            OUTPUT_NAME "${AUSAIOS_OUTPUT_NAME}"
            MACOSX_BUNDLE_BUNDLE_NAME "${AUSAIOS_OUTPUT_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${AUSAIOS_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${AUSAIOS_BUNDLE_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${AUSAIOS_BUNDLE_VERSION}"
            MACOSX_BUNDLE_INFO_PLIST "${_plistoutdir}/Info.plist"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${AUSAIOS_BUNDLE_IDENTIFIER}"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            )

    # --- Embed the .appex ---
    # iOS apps expose their extensions under Bundle/PlugIns/ (no "Contents/"
    # — unlike macOS). The .appex directory is produced by the AUV3 target.
    #
    # Expectation: the caller has already set the AUV3 target's
    # BUNDLE_IDENTIFIER so it's a prefix-child of this host app's bundle ID.
    # installd enforces that relationship on install, and the appex's
    # signed entitlements (application-identifier) must agree with its
    # final CFBundleIdentifier — so any post-build rewrite of that ID is
    # broken on device. The previous version of this function rewrote the
    # ID via PlistBuddy; we stopped doing that and instead require the
    # caller to choose `<host-id>.auv3` up front.
    add_dependencies(${AUSAIOS_TARGET} ${AUSAIOS_AUV3_TARGET})

    set(_appex_src "$<TARGET_BUNDLE_DIR:${AUSAIOS_AUV3_TARGET}>")
    set(_appex_dst "$<TARGET_BUNDLE_DIR:${AUSAIOS_TARGET}>/PlugIns/$<TARGET_PROPERTY:${AUSAIOS_AUV3_TARGET},OUTPUT_NAME>.appex")

    # Embedding is a copy + re-sign of the HOST (its nested-code hash
    # changes when we add the appex). The appex itself keeps the signature
    # Xcode produced. Re-signing uses Xcode's EXPANDED_CODE_SIGN_IDENTITY
    # env var so device builds get the dev cert and simulator builds fall
    # back to ad-hoc.
    set(_ios_embed_script "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/ios_embed_appex.sh")
    add_custom_command(TARGET ${AUSAIOS_TARGET} POST_BUILD
            COMMAND "${_ios_embed_script}"
                "${_appex_src}"
                "${_appex_dst}"
                "$<TARGET_BUNDLE_DIR:${AUSAIOS_TARGET}>"
            COMMENT "Embedding ${AUSAIOS_AUV3_TARGET}.appex into host")
endfunction()
