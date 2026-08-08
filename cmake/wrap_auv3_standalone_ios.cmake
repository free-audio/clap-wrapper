# target_add_auv3_standalone_ios_wrapper — iOS counterpart to
# target_add_auv3_standalone_wrapper (macOS).
#
# Produces an iOS .app bundle that embeds the named AUv3 .appex in
# PlugIns/ and, at launch, instantiates the AU by AudioComponentDescription
# and presents the plugin's UIViewController as the root VC.
#
# For an instrument this app is a shippable standalone synth (audio out,
# MIDI in/out, plugin UI, icon/launch-screen knobs below). Effects are not
# covered: no audio input bus is wired up, see the note in the sources.

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

            # --- Packaging knobs (all optional) ---
            DEVELOPMENT_TEAM           # Apple Developer team ID, applied to host + appex
            ICON_ASSET_CATALOG         # path to a .xcassets directory; required for branding
            APP_ICON_NAME              # image-set name inside the catalog (default "AppIcon")
            LAUNCH_SCREEN_IMAGE        # image-set name in the catalog for UILaunchScreen
            CUSTOM_INFO_PLIST_TEMPLATE # path to a developer-supplied Info.plist.in
            )
    set(multiValueArgs
            EXTRA_INFO_PLIST_ENTRIES   # raw <key>...</key><...>... XML, one stanza per item
            )
    cmake_parse_arguments(AUSAIOS "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

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

    if (NOT DEFINED AUSAIOS_APP_ICON_NAME OR "${AUSAIOS_APP_ICON_NAME}" STREQUAL "")
        set(AUSAIOS_APP_ICON_NAME "AppIcon")
    endif()

    # --- Packaging-knob validation ---
    # Asset catalog is optional; without it the app ships with the iOS
    # placeholder icon. Warn loudly so consumers don't ship that by accident.
    if (NOT DEFINED AUSAIOS_ICON_ASSET_CATALOG OR "${AUSAIOS_ICON_ASSET_CATALOG}" STREQUAL "")
        message(WARNING
            "clap-wrapper: ${AUSAIOS_TARGET} has no ICON_ASSET_CATALOG set — "
            "the iOS app will ship with the system placeholder icon. "
            "Pass ICON_ASSET_CATALOG <path-to-.xcassets> to brand it.")
    else()
        if (NOT EXISTS "${AUSAIOS_ICON_ASSET_CATALOG}")
            message(FATAL_ERROR
                "clap-wrapper: ICON_ASSET_CATALOG path does not exist: "
                "${AUSAIOS_ICON_ASSET_CATALOG}")
        endif()
    endif()

    if (DEFINED AUSAIOS_LAUNCH_SCREEN_IMAGE AND NOT "${AUSAIOS_LAUNCH_SCREEN_IMAGE}" STREQUAL "")
        if (NOT DEFINED AUSAIOS_ICON_ASSET_CATALOG OR "${AUSAIOS_ICON_ASSET_CATALOG}" STREQUAL "")
            message(FATAL_ERROR
                "clap-wrapper: LAUNCH_SCREEN_IMAGE \"${AUSAIOS_LAUNCH_SCREEN_IMAGE}\" "
                "is set but ICON_ASSET_CATALOG is not — the launch image must "
                "live inside an asset catalog. Pass ICON_ASSET_CATALOG.")
        endif()
        set(AUSAIOS_LAUNCH_SCREEN_PLIST_BODY
            "<key>UIImageName</key>\n        <string>${AUSAIOS_LAUNCH_SCREEN_IMAGE}</string>")
    else()
        # Preserve the prior default — empty UIColorName = system background.
        set(AUSAIOS_LAUNCH_SCREEN_PLIST_BODY
            "<key>UIColorName</key>\n        <string></string>")
    endif()

    # --- Info.plist composition ---
    # Two extension points:
    #   CUSTOM_INFO_PLIST_TEMPLATE — point at a developer-maintained
    #     Info.plist.in. The wrapper still runs configure_file with the
    #     full set of AUSAIOS_* substitution variables, so a custom
    #     template can opt in to whatever it needs.
    #   EXTRA_INFO_PLIST_ENTRIES — raw XML stanzas inserted before the
    #     closing </dict>. Each item is one stanza like
    #     "<key>Foo</key><string>Bar</string>". Use this for one-off keys
    #     (privacy descriptions, file-sharing flags, App Store flags)
    #     without having to fork the template.
    if (DEFINED AUSAIOS_CUSTOM_INFO_PLIST_TEMPLATE AND
        NOT "${AUSAIOS_CUSTOM_INFO_PLIST_TEMPLATE}" STREQUAL "")
        if (NOT EXISTS "${AUSAIOS_CUSTOM_INFO_PLIST_TEMPLATE}")
            message(FATAL_ERROR
                "clap-wrapper: CUSTOM_INFO_PLIST_TEMPLATE path does not exist: "
                "${AUSAIOS_CUSTOM_INFO_PLIST_TEMPLATE}")
        endif()
        set(_plist_template "${AUSAIOS_CUSTOM_INFO_PLIST_TEMPLATE}")
        message(STATUS "clap-wrapper: ${AUSAIOS_TARGET} using custom Info.plist template: ${_plist_template}")
    else()
        set(_plist_template "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/ios/auv3/Info.plist.in")
    endif()

    if (DEFINED AUSAIOS_EXTRA_INFO_PLIST_ENTRIES AND
        NOT "${AUSAIOS_EXTRA_INFO_PLIST_ENTRIES}" STREQUAL "")
        list(JOIN AUSAIOS_EXTRA_INFO_PLIST_ENTRIES "\n    " AUSAIOS_EXTRA_INFO_PLIST_ENTRIES)
    else()
        set(AUSAIOS_EXTRA_INFO_PLIST_ENTRIES "")
    endif()

    message(STATUS "clap-wrapper: Adding AUv3 iOS Standalone to target ${AUSAIOS_TARGET} for '${AUSAIOS_OUTPUT_NAME}'")

    # --- Info.plist ---
    set(_plistoutdir "${CMAKE_CURRENT_BINARY_DIR}/${AUSAIOS_TARGET}-plist")
    file(MAKE_DIRECTORY "${_plistoutdir}")
    configure_file(
        "${_plist_template}"
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

    # --- In-process AU instantiation ---
    # On iOS 18+ third-party AUv3 extensions are filtered out of every host
    # process except GarageBand-class Apple-blessed ones, even for the host's
    # own embedded appex. componentsMatchingDescription: / AudioComponent-
    # FindNext / AVAudioUnit instantiateWithComponentDescription: all return
    # zero third-party matches. Rather than fight that sandbox, we link the
    # AUv3 wrapper runtime + generated factory class into the host too and
    # instantiate the factory directly as ObjC — no registry lookup.
    #
    # The hosted CLAP engine stays shared on disk: wrap_auv3.cmake already
    # compiles the factory class into the .appex via configure_file, and the
    # consuming project packages the CLAP as a .framework that both the
    # .appex and host link against. Here we just pull the wrapper sources
    # into the host too with the same per-plugin defines.
    target_sources(${AUSAIOS_TARGET} PRIVATE
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasauv3.mm"
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/auv3_audiounit.mm"
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/auv3_parameters.mm"
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/process.mm"
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/os/macos.mm")

    # generated_auv3_entrypoints.hxx is produced (via configure_file on iOS)
    # by target_add_auv3_wrapper into this directory for the .appex target.
    # Reusing it for the host keeps one source of truth for the factory
    # class name.
    set(_host_auv3_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/${AUSAIOS_AUV3_TARGET}-auv3-build-helper-output")
    target_include_directories(${AUSAIOS_TARGET} PRIVATE "${_host_auv3_gen_dir}")

    # clap-wrapper-auv3-lib is an INTERFACE library created by
    # target_add_auv3_wrapper; it propagates the clap-wrapper headers and
    # the `clap` library itself. Requires that wrapper was called on
    # AUSAIOS_AUV3_TARGET before this function.
    if (NOT TARGET ${AUSAIOS_AUV3_TARGET}-clap-wrapper-auv3-lib)
        message(FATAL_ERROR
            "clap-wrapper: target_add_auv3_wrapper must be called on "
            "${AUSAIOS_AUV3_TARGET} before target_add_auv3_standalone_ios_wrapper")
    endif()
    target_link_libraries(${AUSAIOS_TARGET} PRIVATE
            ${AUSAIOS_AUV3_TARGET}-clap-wrapper-auv3-lib)

    # Reproduce wrap_auv3.cmake's factory-class-name hash so the host can
    # NSClassFromString() the same symbol the appex exports. MD5 of
    # manufacturer+subtype, first 8 chars, uppercased.
    string(MD5 _host_auv3_md5_full "${AUSAIOS_AU_MANUFACTURER}${AUSAIOS_AU_SUBTYPE}")
    string(SUBSTRING "${_host_auv3_md5_full}" 0 8 _host_auv3_md5_short)
    string(TOUPPER "${_host_auv3_md5_short}" _host_auv3_md5_short)
    set(_host_factory_class "ClapAUv3VC_${_host_auv3_md5_short}")

    target_compile_definitions(${AUSAIOS_TARGET} PRIVATE
            STATICALLY_LINKED_CLAP_ENTRY=1
            AUV3_FACTORY_CLASS_NAME_STR="${_host_factory_class}")

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

    # --- Signing ---
    # Apply DEVELOPMENT_TEAM to host AND appex. They must agree or the
    # embedded appex won't validate against the host on device install.
    # If unset, leave the attribute alone so a global
    # -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=... still falls through.
    if (DEFINED AUSAIOS_DEVELOPMENT_TEAM AND NOT "${AUSAIOS_DEVELOPMENT_TEAM}" STREQUAL "")
        set_target_properties(${AUSAIOS_TARGET} PROPERTIES
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${AUSAIOS_DEVELOPMENT_TEAM}")
        set_target_properties(${AUSAIOS_AUV3_TARGET} PROPERTIES
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${AUSAIOS_DEVELOPMENT_TEAM}")
    endif()

    # --- App icon / launch screen ---
    # The asset catalog is added as a Resource of the host target; Xcode's
    # asset-catalog compiler runs actool on it and injects the right
    # CFBundleIcons / CFBundleIcons~ipad keys at build time, so no plist
    # substitution is needed for the icon. The launch screen is plist-side
    # only (UILaunchScreen → UIImageName is computed above).
    if (DEFINED AUSAIOS_ICON_ASSET_CATALOG AND NOT "${AUSAIOS_ICON_ASSET_CATALOG}" STREQUAL "")
        target_sources(${AUSAIOS_TARGET} PRIVATE "${AUSAIOS_ICON_ASSET_CATALOG}")
        set_source_files_properties("${AUSAIOS_ICON_ASSET_CATALOG}" PROPERTIES
                MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${AUSAIOS_TARGET} PROPERTIES
                XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "${AUSAIOS_APP_ICON_NAME}")
    endif()

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
