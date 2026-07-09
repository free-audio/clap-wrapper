
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

            ENTITLEMENTS  # optional: per-plugin entitlements file (macOS appex signing)

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

    if (NOT CMAKE_GENERATOR STREQUAL "Xcode")
        message(FATAL_ERROR "clap-wrapper: AUv3 requires the Xcode generator (-G Xcode). "
                "The appex must be linked as an app-extension product (entry point "
                "_NSExtensionMain) and signed by Xcode; Ninja/Makefiles cannot produce a "
                "loadable AUv3 appex. Remove AUV3 from PLUGIN_FORMATS (or don't set "
                "CLAP_WRAPPER_BUILD_AUV3) when using this generator.")
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

    set(bhtgoutdir "${CMAKE_CURRENT_BINARY_DIR}/${AUV3_TARGET}-auv3-build-helper-output")
    file(MAKE_DIRECTORY "${bhtgoutdir}")

    if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # macOS: build a host tool that reads info from the hosted CLAP and
        # emits the real Info.plist + generated entrypoints at build time.
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

        # Placeholder Info.plist for the CMake generate step (real one is
        # produced by the helper at build time, POST_BUILD).
        if (NOT EXISTS "${bhtgoutdir}/auv3_Info.plist")
            file(WRITE "${bhtgoutdir}/auv3_Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundlePackageType</key>
    <string>XPC!</string>
</dict>
</plist>
")
        endif()

        add_custom_command(TARGET ${bhtg} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "clap-wrapper: auv3 configuration output dir is ${bhtgoutdir}"
                )

        add_dependencies(${AUV3_TARGET} ${bhtg})
    else()
        # iOS: the build-helper is a host C++ tool and Xcode cannot target
        # both macOS and iOS from a single CMake configure. So on iOS we
        # bypass the helper and generate the two files it emits directly
        # from CMake templates. This limits us to the single-plugin case
        # (the most common), which matches what the clap-first flow does
        # on iOS anyway — a single .clap linked into the .appex.
        if (NOT DEFINED AUV3_INSTRUMENT_TYPE)
            set(AUV3_INSTRUMENT_TYPE "aumu")
        endif()
        if (NOT DEFINED AUV3_SUBTYPE_CODE)
            set(AUV3_SUBTYPE_CODE "errr")
        endif()
        if (NOT DEFINED AUV3_MANUFACTURER_CODE)
            set(AUV3_MANUFACTURER_CODE "errr")
        endif()
        if (NOT DEFINED AUV3_MANUFACTURER_NAME)
            set(AUV3_MANUFACTURER_NAME "errr")
        endif()
        if (NOT DEFINED AUV3_OUTPUT_NAME)
            set(AUV3_OUTPUT_NAME ${AUV3_TARGET})
        endif()

        # Derive a deterministic ObjC class-name suffix so multiple wrappers
        # in the same process don't collide. Host tool uses FNV-1a + an LCG
        # mix; MD5 here isn't bit-identical but serves the same purpose
        # (deterministic + unique per plugin).
        string(MD5 _csd_md5_full "${AUV3_MANUFACTURER_CODE}${AUV3_SUBTYPE_CODE}")
        string(SUBSTRING "${_csd_md5_full}" 0 8 _csd_md5_short)
        string(TOUPPER "${_csd_md5_short}" _csd_md5_short)
        set(AUV3_FACTORY_CLASS_NAME "ClapAUv3VC_${_csd_md5_short}")

        # Pack dotted version into uint32 (matches bundleversToVersion,
        # including the per-byte clamp so the packing stays ordered when a
        # component exceeds 255).
        set(_vmaj 0)
        set(_vmin 0)
        set(_vpat 0)
        if (AUV3_BUNDLE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
            set(_vmaj ${CMAKE_MATCH_1})
            set(_vmin ${CMAKE_MATCH_2})
            set(_vpat ${CMAKE_MATCH_3})
        elseif (AUV3_BUNDLE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)")
            set(_vmaj ${CMAKE_MATCH_1})
            set(_vmin ${CMAKE_MATCH_2})
        endif()
        foreach(_vc _vmaj _vmin _vpat)
            if (${${_vc}} GREATER 255)
                set(${_vc} 255)
            endif()
        endforeach()
        math(EXPR AUV3_VERSION_INT "(${_vmaj} * 65536) + (${_vmin} * 256) + ${_vpat}")
        if (AUV3_VERSION_INT EQUAL 0)
            set(AUV3_VERSION_INT 1)
        endif()

        # The hosted CLAP's name (for Clap::getValidCLAPSearchPaths fallback)
        # and id. On iOS we're always clap-first (static-linked), so the
        # runtime path through _library.hasEntryPoint() is used — clap-name
        # and clap-id are only cosmetic on iOS, but we still fill them.
        if (NOT DEFINED AUV3_CLAP_NAME)
            set(AUV3_CLAP_NAME "${AUV3_OUTPUT_NAME}")
        endif()
        if (NOT DEFINED AUV3_CLAP_ID)
            set(AUV3_CLAP_ID "")
        endif()

        # Deployment target for the Info.plist MinimumOSVersion key.
        if (DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
            set(AUV3_IOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(AUV3_IOS_DEPLOYMENT_TARGET "15.0")
        endif()

        # The template below bakes the bundle identifier in at configure
        # time, so the empty-identifier fallback must be resolved HERE —
        # the shared fallback further down runs after this configure_file
        # and would leave an empty CFBundleIdentifier in the plist while
        # PRODUCT_BUNDLE_IDENTIFIER gets the generated default (a mismatch
        # installd/pluginkit reject).
        if ("${AUV3_BUNDLE_IDENTIFIER}" STREQUAL "")
            string(MAKE_C_IDENTIFIER ${AUV3_OUTPUT_NAME} _ios_outidentifier)
            string(REPLACE "_" "-" _ios_repout ${_ios_outidentifier})
            set(AUV3_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${_ios_repout}.auv3")
        endif()

        configure_file(
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/templates/auv3_ios_Info.plist.in"
            "${bhtgoutdir}/auv3_Info.plist"
            @ONLY)
        configure_file(
            "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/templates/generated_auv3_entrypoints.hxx.in"
            "${bhtgoutdir}/generated_auv3_entrypoints.hxx"
            @ONLY)
    endif()

    # The three dispatch branches below schedule the build-helper to emit
    # auv3_Info.plist + generated_auv3_entrypoints.hxx as POST_BUILD steps
    # of the ${bhtg} target. On iOS we already produced those files via
    # configure_file() above, so the POST_BUILD dispatch is macOS-only.
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # Everything is prepared. Skip the host-tool POST_BUILD dispatch.
    elseif (DEFINED AUV3_CLAP_TARGET_FOR_CONFIG)
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
    # auv3_infoplist_top is consumed by the macOS host build-helper; on iOS
    # we don't build the helper so this template is not needed.
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        configure_file(${bhsc}/auv3_infoplist_top.in
                ${bhtgoutdir}/auv3_infoplist_top)
    endif()

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

    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # On iOS the appex can't dlopen a .clap, so the hosted CLAP's
        # clap_entry must be statically linked into the appex binary.
        # This compile def flips Clap::Library's constructor to take the
        # extern const clap_entry pointer directly instead of searching the
        # filesystem via CLAP search paths (which don't exist on iOS).
        # The consuming project is expected to pull a clap_entry-defining
        # TU into the appex target (clap-saw-demo does this via
        # src/clap-saw-demo-clap-entry.cpp).
        target_compile_definitions(${AUV3_TARGET} PRIVATE STATICALLY_LINKED_CLAP_ENTRY=1)
    endif()

    if (NOT TARGET ${AUV3_TARGET}-clap-wrapper-auv3-lib)
        add_library(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE)
        target_include_directories(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE "${bhtgoutdir}" "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE clap)
        target_link_libraries(${AUV3_TARGET}-clap-wrapper-auv3-lib INTERFACE clap-wrapper-extensions clap-wrapper-shared-detail clap-wrapper-compile-options)
    endif ()

    set_target_properties(${AUV3_TARGET} PROPERTIES
            OUTPUT_NAME "${AUV3_OUTPUT_NAME}"
            LIBRARY_OUTPUT_NAME "${AUV3_OUTPUT_NAME}")  # also set for target_copy_after_build compatibility
    target_link_libraries(${AUV3_TARGET} PUBLIC ${AUV3_TARGET}-clap-wrapper-auv3-lib)

    if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
        set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
    endif ()

    # On iOS, target both iPhone (1) and iPad (2). Without this, Xcode
    # defaults to iPhone-only and the appex won't register for iPad hosts
    # (Matches in registry: 0, even with a correct Info.plist).
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(${AUV3_TARGET} PROPERTIES
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    endif()

    # Frameworks: AppKit on macOS, UIKit on iOS. Everything else is shared.
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${AUV3_TARGET} PUBLIC
                "-framework Foundation"
                "-framework CoreFoundation"
                "-framework UIKit"
                "-framework AudioToolbox"
                "-framework AVFoundation"
                "-framework CoreAudio"
                "-framework CoreAudioKit"
                "-framework CoreMIDI")
    else()
        target_link_libraries(${AUV3_TARGET} PUBLIC
                "-framework Foundation"
                "-framework CoreFoundation"
                "-framework AppKit"
                "-framework AudioToolbox"
                "-framework AVFoundation"
                "-framework CoreAudio"
                "-framework CoreAudioKit"
                "-framework CoreMIDI")
    endif()

    set_target_properties(${AUV3_TARGET} PROPERTIES
            MACOSX_BUNDLE True
            BUNDLE_EXTENSION appex
            OUTPUT_NAME ${AUV3_OUTPUT_NAME}
            MACOSX_BUNDLE_GUI_IDENTIFIER "${AUV3_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_NAME ${AUV3_OUTPUT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION ${AUV3_BUNDLE_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${AUV3_BUNDLE_VERSION}
            )

    # The build-helper (macOS) or CMake configure_file (iOS) produces
    # auv3_Info.plist. Feed it to Xcode as the target's own INFOPLIST_FILE
    # input: ProcessInfoPlistFile then reads OUR file whenever it (re)stamps
    # the bundle plist and itself adds the DT* / CFBundleSupportedPlatforms
    # keys pluginkit requires on iOS. On macOS the target dependency on the
    # build-helper guarantees the file is written before it is read; on iOS
    # it exists from configure time.
    #
    # Do NOT rewrite/merge the bundle's Info.plist POST_BUILD instead:
    # Xcode re-runs ProcessInfoPlistFile on incremental builds when it sees
    # its output was modified, and the scheduling between that task and
    # script phases is not deterministic — a rewritten plist randomly loses
    # NSExtension/AudioComponents and the appex silently stops registering.
    if (CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(${AUV3_TARGET} PROPERTIES
                XCODE_PRODUCT_TYPE com.apple.product-type.app-extension
                )
    endif()
    set_target_properties(${AUV3_TARGET} PROPERTIES
            XCODE_ATTRIBUTE_INFOPLIST_FILE "${bhtgoutdir}/auv3_Info.plist")

    set_target_properties(${AUV3_TARGET} PROPERTIES XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${AUV3_BUNDLE_IDENTIFIER}")

    # Embedding a .clap inside Contents/PlugIns/ is a macOS-only story:
    #   - macOS AUv3 appex's Contents/PlugIns/ is a valid search path that
    #     clap-wrapper's runtime (auv3_audiounit.mm) will dlopen from.
    #   - iOS app extensions may only load code from the signed .appex
    #     bundle's Frameworks/ directory, not Contents/PlugIns/, and the
    #     hosted CLAP must be a Framework (not a .clap MODULE). iOS therefore
    #     requires the clap-first / static-linked path: the AUV3 target links
    #     the impl lib and the CLAP symbols are baked into the appex binary.
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        macos_include_clap_in_bundle(TARGET ${AUV3_TARGET}
                MACOS_EMBEDDED_CLAP_LOCATION ${AUV3_MACOSX_EMBEDDED_CLAP_LOCATION})
        macos_bundle_flag(TARGET ${AUV3_TARGET})
    endif()

    if(NOT AUV3_RESOURCE_DIRECTORY STREQUAL "")
        message(WARNING "RESOURCE_DIRECTORY defined, but not (yet) supported for AUV3")
    endif()

    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # iOS appex signing is handled by Xcode at build time via the selected
        # code-sign identity + provisioning profile (or the simulator's
        # automatic signing). We do NOT run `codesign -s -` here: ad-hoc
        # signing against the iOS code directory produces a bundle the
        # simulator and devices will refuse to load. Also, iOS has no
        # app-sandbox entitlement — it's implicit.
        # The default Xcode settings give us ad-hoc signing on the simulator
        # (no provisioning profile required).
    else()
        # Set entitlements for sandboxing (required for AUv3 registration on
        # macOS). Plugins can pass their own file via ENTITLEMENTS; the
        # shipped default is minimal (app-sandbox + user-selected files).
        if (NOT DEFINED AUV3_ENTITLEMENTS)
            set(AUV3_ENTITLEMENTS "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/auv3/auv3.entitlements")
        endif()

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
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${AUV3_TARGET} FLAVOR auv3)
    endif ()
endfunction(target_add_auv3_wrapper)
