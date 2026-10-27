# The standalone's audio and MIDI run on the platform's own apis on macOS (CoreAudio,
# CoreMIDI) and Windows (WASAPI, WinMM). This switch takes RtAudio and RtMidi for both in
# their place, as every standalone did before. Linux has PipeWire and JACK for audio and
# the ALSA sequencer for MIDI, each loaded at run time and built where its headers are
# found; without the PipeWire and JACK headers it takes RtAudio and RtMidi, as this
# switch does. Windows built with RtAudio's ASIO
# (RTAUDIO_API_ASIO) takes ASIO from RtAudio next to the native WASAPI either way.
option(CLAP_WRAPPER_STANDALONE_RTAUDIO
       "Standalone: RtAudio and RtMidi in place of CoreAudio/CoreMIDI, WASAPI/WinMM and PipeWire/JACK/ALSA"
       OFF)

# On Linux the standalone can put a strip above the plugin's window: the button that opens
# its Audio/MIDI settings, and what the audio runs at or the error that stopped it. It is
# left out unless this switch is turned off, so the window is the plugin's own size and the
# plugin opens the settings from its own ui (show_audio_midi_configuration_screen, standalone
# features). A plugin without a gui always gets the strip, the window's only content.
# Nothing changes on macOS and Windows, whose standalones have their menus instead.
option(CLAP_WRAPPER_STANDALONE_X11_HIDE_STRIP
       "Standalone on Linux: no strip above the plugin (the Audio/MIDI settings button and the audio's status)"
       ON)

function(target_add_standalone_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            BUNDLE_IDENTIFIER
            BUNDLE_VERSION
            RESOURCE_DIRECTORY

            PLUGIN_INDEX
            PLUGIN_ID
            STATICALLY_LINKED_CLAP_ENTRY
            HOSTED_CLAP_NAME

            WINDOWS_ICON
            MACOS_ICON

            MACOS_EMBEDDED_CLAP_LOCATION
            )
    set(multiValueArgs
            URL_SCHEMES  # schemes a link opens the standalone with; the plugin gets the url
            )
    cmake_parse_arguments(SA "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )

    if (NOT DEFINED SA_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_standalone_wrapper requires a target")
    endif()

    if (NOT TARGET ${SA_TARGET})
        message(FATAL_ERROR "clap-wrapper: standalone-target must be a target")
    endif()

    if (NOT DEFINED SA_PLUGIN_ID)
        set(SA_PLUGIN_ID "")
    endif()

    if (NOT DEFINED SA_PLUGIN_INDEX)
        set(SA_PLUGIN_INDEX 0)
    endif()

    if (NOT DEFINED SA_OUTPUT_NAME)
        set(SA_OUTPUT_NAME ${SA_TARGET})
    endif()

    if (NOT DEFINED SA_BUNDLE_IDENTIFIER)
        # What Info.plist.in used to hardcode. Kept as the default so a caller
        # that does not pass one keeps the identifier it already shipped.
        set(SA_BUNDLE_IDENTIFIER "${SA_OUTPUT_NAME}.standalone")
    endif()

    if (NOT DEFINED SA_WINDOWS_ICON)
        set(SA_WINDOWS_ICON "")
    endif()

    if (NOT DEFINED SA_MACOS_ICON)
        set(SA_MACOS_ICON "")
    endif()

    if (NOT DEFINED SA_RESOURCE_DIRECTORY)
        set(SA_RESOURCE_DIRECTORY "")
    endif()

    if (NOT DEFINED SA_BUNDLE_VERSION)
        message(STATUS "No SA_BUNDLE_VERSION - using ${PROJECT_VERSION}")
        set(SA_BUNDLE_VERSION "${PROJECT_VERSION}")
    endif()

    # the url schemes a link opens the standalone with (open_url, standalone features):
    # declared in the macos bundle's Info.plist, and on every platform the ones the
    # standalone takes from the system and hands the plugin. lowercase, as a scheme is
    # compared without case
    set(sa_url_schemes "")
    foreach(_scheme IN LISTS SA_URL_SCHEMES)
        string(TOLOWER "${_scheme}" _scheme)
        if (NOT _scheme MATCHES "^[a-z][a-z0-9+.-]*$")
            message(FATAL_ERROR "clap-wrapper: '${_scheme}' in URL_SCHEMES is not a url scheme: a letter, then letters, digits, '+', '-' or '.'")
        endif()
        list(APPEND sa_url_schemes "${_scheme}")
    endforeach()
    list(REMOVE_DUPLICATES sa_url_schemes)

    # RtAudio and RtMidi for everything, or RtAudio for ASIO only
    set(sa_rtaudio ${CLAP_WRAPPER_STANDALONE_RTAUDIO})

    # linux: pipewire and jack, each where its headers are. the libraries are dlopen()ed,
    # so that one binary runs with either, both or neither installed
    set(sa_pipewire OFF)
    set(sa_jack OFF)
    if (NOT APPLE AND NOT WIN32 AND NOT sa_rtaudio)
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(CLAP_WRAPPER_PIPEWIRE QUIET libpipewire-0.3)
        endif()
        if (CLAP_WRAPPER_PIPEWIRE_FOUND)
            set(sa_pipewire ON)
        else()
            message(STATUS "clap-wrapper: no PipeWire headers (libpipewire-0.3-dev); the standalone is built without PipeWire")
        endif()

        find_path(CLAP_WRAPPER_JACK_INCLUDE_DIR jack/jack.h)
        if (CLAP_WRAPPER_JACK_INCLUDE_DIR)
            set(sa_jack ON)
        else()
            message(STATUS "clap-wrapper: no JACK headers (libjack-jackd2-dev); the standalone is built without JACK")
        endif()

        if (NOT sa_pipewire AND NOT sa_jack)
            message(STATUS "clap-wrapper: neither PipeWire nor JACK headers; the standalone takes RtAudio for audio")
            set(sa_rtaudio ON)
        endif()
    endif()

    # RtMidi with the RtAudio switch; else linux's MIDI is the ALSA sequencer, where its
    # headers are, and no MIDI at all where they are not. a linux build that takes RtAudio
    # for want of the PipeWire and JACK headers keeps the ALSA sequencer
    set(sa_rtmidi ${CLAP_WRAPPER_STANDALONE_RTAUDIO})
    set(sa_alsaseq OFF)
    if (NOT APPLE AND NOT WIN32 AND NOT sa_rtmidi)
        find_path(CLAP_WRAPPER_ALSA_INCLUDE_DIR alsa/asoundlib.h)
        if (CLAP_WRAPPER_ALSA_INCLUDE_DIR)
            set(sa_alsaseq ON)
        else()
            message(STATUS "clap-wrapper: no ALSA headers (libasound2-dev); the standalone is built without MIDI")
        endif()
    endif()
    set(sa_asio OFF)
    if (WIN32 AND RTAUDIO_API_ASIO)
        set(sa_asio ON)
    endif()

    set(sasrc ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone)
    set(salib ${SA_TARGET}-clap-wrapper-standalone-lib)
    add_library(${salib} STATIC
            ${sasrc}/entry.cpp
            ${sasrc}/standalone_host.cpp
            ${sasrc}/standalone_host_audio.cpp
            ${sasrc}/standalone_host_midi.cpp
            ${sasrc}/standalone_host_features.cpp
            ${sasrc}/standalone_settings.cpp
            ${sasrc}/standalone_audio.cpp
            ${sasrc}/standalone_backends.cpp
            )
    target_link_libraries(${salib}
            PUBLIC
            clap-wrapper-shared-detail
            )
    target_link_libraries(${salib} PRIVATE clap-wrapper-compile-options)
    # one string, the schemes separated by commas; empty for a standalone no link opens
    string(REPLACE ";" "," sa_url_schemes_joined "${sa_url_schemes}")
    target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_URL_SCHEMES="${sa_url_schemes_joined}")
    # the backends each keep file-local helpers, some with the same names
    set_target_properties(${salib} PROPERTIES UNITY_BUILD OFF)

    if (sa_rtaudio OR sa_asio)
        guarantee_rtaudio()
        target_sources(${salib} PRIVATE ${sasrc}/rtaudio_backend.cpp)
        target_link_libraries(${salib} PUBLIC base-sdk-rtaudio)
    endif()
    if (sa_rtaudio)
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_RTAUDIO=1)
    endif()
    if (sa_rtmidi)
        guarantee_rtmidi()
        target_sources(${salib} PRIVATE ${sasrc}/rtmidi_backend.cpp)
        target_link_libraries(${salib} PUBLIC base-sdk-rtmidi)
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_RTMIDI=1)
    endif()
    if (sa_pipewire)
        target_sources(${salib} PRIVATE ${sasrc}/linux/pipewire_backend.cpp)
        target_include_directories(${salib} SYSTEM PRIVATE ${CLAP_WRAPPER_PIPEWIRE_INCLUDE_DIRS})
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_PIPEWIRE=1)
    endif()
    if (sa_jack)
        target_sources(${salib} PRIVATE ${sasrc}/linux/jack_backend.cpp)
        target_include_directories(${salib} SYSTEM PRIVATE ${CLAP_WRAPPER_JACK_INCLUDE_DIR})
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_JACK=1)
    endif()
    if (sa_alsaseq)
        target_sources(${salib} PRIVATE ${sasrc}/linux/alsaseq_midi_backend.cpp)
        target_include_directories(${salib} SYSTEM PRIVATE ${CLAP_WRAPPER_ALSA_INCLUDE_DIR})
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_ALSASEQ=1)
    endif()
    if (sa_pipewire OR sa_jack OR sa_alsaseq)
        target_link_libraries(${salib} PRIVATE ${CMAKE_DL_LIBS})
    endif()

    if (APPLE)
        # the device listing is coreaudio's in both cases: rtaudio takes from it what it
        # cannot tell itself
        target_sources(${salib} PRIVATE ${sasrc}/macos/coreaudio_backend.cpp)
        if (NOT sa_rtmidi)
            target_sources(${salib} PRIVATE ${sasrc}/macos/coremidi_backend.cpp)
        endif()
        target_link_libraries(${salib}
                PUBLIC "-framework AVFoundation" "-framework Foundation" "-framework CoreFoundation" "-framework AppKit"
                "-framework CoreAudio" "-framework CoreMIDI")
    elseif (WIN32 AND NOT sa_rtaudio)
        target_sources(${salib} PRIVATE
                ${sasrc}/windows/wasapi_backend.cpp
                ${sasrc}/windows/winmm_midi_backend.cpp)
        target_link_libraries(${salib} PUBLIC winmm avrt ole32)
    endif()

    if (APPLE)
        set(MAIN_XIB "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/MainMenu.xib")
        set(GEN_XIB "${CMAKE_BINARY_DIR}/generated_xib/${SA_TARGET}/MainMenu.xib")
        configure_file(${MAIN_XIB} ${GEN_XIB})

        # the Info.plist template with the url schemes in it, one per target as the xib.
        # cmake fills in the template's ${} and @@ itself when it makes the bundle, so the
        # schemes go in by plain replacement, and what cmake fills in is left for it
        set(MAIN_PLIST "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/Info.plist.in")
        set(GEN_PLIST "${CMAKE_BINARY_DIR}/generated_plist/${SA_TARGET}/Info.plist.in")
        set(sa_url_types "")
        if (sa_url_schemes)
            string(CONCAT sa_url_types
                    "<key>CFBundleURLTypes</key>\n"
                    "\t<array>\n"
                    "\t\t<dict>\n"
                    "\t\t\t<key>CFBundleURLName</key>\n"
                    "\t\t\t<string>\${MACOSX_BUNDLE_GUI_IDENTIFIER}</string>\n"
                    "\t\t\t<key>CFBundleTypeRole</key>\n"
                    "\t\t\t<string>Viewer</string>\n"
                    "\t\t\t<key>CFBundleURLSchemes</key>\n"
                    "\t\t\t<array>\n")
            foreach(_scheme IN LISTS sa_url_schemes)
                string(APPEND sa_url_types "\t\t\t\t<string>${_scheme}</string>\n")
            endforeach()
            string(APPEND sa_url_types
                    "\t\t\t</array>\n"
                    "\t\t</dict>\n"
                    "\t</array>")
        endif()
        file(READ "${MAIN_PLIST}" sa_plist)
        string(REPLACE "<!-- clap-wrapper: url types -->" "${sa_url_types}" sa_plist "${sa_plist}")
        # written only when it changed, so that the bundle is not remade on every configure
        file(WRITE "${GEN_PLIST}.tmp" "${sa_plist}")
        configure_file("${GEN_PLIST}.tmp" "${GEN_PLIST}" COPYONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MAIN_PLIST}")

        target_sources(${SA_TARGET} PRIVATE
                "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone.mm"
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/AppDelegate.mm
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/StandaloneFunctions.mm
                ${GEN_XIB}
                )


        set_target_properties(${SA_TARGET} PROPERTIES
                BUNDLE TRUE
                BUNDLE_NAME ${SA_OUTPUT_NAME}
                BUNDLE_EXTENSION app
                OUTPUT_NAME ${SA_OUTPUT_NAME}
                MACOSX_BUNDLE_BUNDLE_NAME ${SA_OUTPUT_NAME}
                MACOSX_BUNDLE_GUI_IDENTIFIER "${SA_BUNDLE_IDENTIFIER}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING ${SA_BUNDLE_VERSION}
                MACOSX_BUNDLE_LONG_VERSION_STRING ${SA_BUNDLE_VERSION}
                MACOSX_BUNDLE_BUNDLE_VERSION ${SA_BUNDLE_VERSION}
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_INFO_PLIST ${GEN_PLIST}
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${SA_BUNDLE_IDENTIFIER}"
                RESOURCE "${GEN_XIB}"
                )

        if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
            message(STATUS "cmake-wrapper: ejecting xib->nib rules manually for ${CMAKE_GENERATOR} on ${SA_TARGET}")
            find_program(IBTOOL ibtool REQUIRED)
            add_custom_command(TARGET ${SA_TARGET} PRE_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo ${IBTOOL} --compile "$<TARGET_FILE_DIR:${SA_TARGET}>/../Resources/MainMenu.nib" ${GEN_XIB}
                    COMMAND ${IBTOOL} --compile "$<TARGET_FILE_DIR:${SA_TARGET}>/../Resources/MainMenu.nib" ${GEN_XIB}
                    )
        endif()

        if(NOT "${SA_MACOS_ICON}" STREQUAL "")
            add_custom_command(TARGET ${SA_TARGET} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy ${SA_MACOS_ICON} "$<TARGET_FILE_DIR:${SA_TARGET}>/../Resources/Icon.icns"
            )
        endif()

        macos_include_clap_in_bundle(TARGET ${SA_TARGET}
                MACOS_EMBEDDED_CLAP_LOCATION ${SA_MACOS_EMBEDDED_CLAP_LOCATION})

    elseif(WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
        guarantee_wil()

        if(NOT "${SA_WINDOWS_ICON}" STREQUAL "")
            file(WRITE "${CMAKE_BINARY_DIR}/generated_icons/windows_standalone_${SA_TARGET}.rc" "1 ICON \"windows_standalone_${SA_TARGET}.ico\"")
            file(COPY_FILE ${SA_WINDOWS_ICON} "${CMAKE_BINARY_DIR}/generated_icons/windows_standalone_${SA_TARGET}.ico")
            target_sources(${SA_TARGET} PRIVATE
                "${CMAKE_BINARY_DIR}/generated_icons/windows_standalone_${SA_TARGET}.rc"
            )
        endif()

        target_sources(${SA_TARGET} PRIVATE
                "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone_windows.cpp"
                "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/windows/windows_standalone.cpp"
                "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/windows/windows_standalone.manifest"
                )

        target_compile_definitions(${salib} PUBLIC
                CLAP_WRAPPER_HAS_WIN32
                UNICODE
                NOMINMAX
                )

        set_target_properties(${SA_TARGET} PROPERTIES
                WIN32_EXECUTABLE TRUE
                OUTPUT_NAME ${SA_OUTPUT_NAME}
                )

        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_definitions(${SA_TARGET} PRIVATE _SILENCE_CLANG_COROUTINE_MESSAGE)
        endif()

        # RuntimeObject was only ever needed for the C++/WinRT JSON parser the
        # settings code used to use; the shared settings store replaced it.
        # rtaudio built with asio: the settings window offers the driver's control panel
        if(sa_asio)
            target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_ASIO=1)
        endif()

        # Dwmapi and UxTheme give the settings window its dark title bar and controls
        target_link_libraries(${SA_TARGET} PRIVATE base-sdk-wil ComCtl32.Lib Dwmapi.lib UxTheme.lib)

    elseif(UNIX)
        target_sources(${SA_TARGET} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone.cpp)

        message(STATUS "clap-wrapper: Using Standalone X11 gui for CLAP Wrapper")
        target_link_libraries(${salib} PUBLIC X11)
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_X11)
        if(CLAP_WRAPPER_STANDALONE_X11_HIDE_STRIP)
            target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_X11_HIDE_STRIP=1)
        endif()
        target_sources(${salib} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/x11_gui.cpp
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/x11_ui.cpp
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/x11_settings.cpp)

        set_target_properties(${SA_TARGET} PROPERTIES OUTPUT_NAME ${SA_OUTPUT_NAME})

    else()
        target_sources(${SA_TARGET} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone.cpp)
    endif()

    # Copy resource directory, if defined
    if(NOT SA_RESOURCE_DIRECTORY STREQUAL "")
        message(WARNING "RESOURCE_DIRECTORY defined, but not (yet) supported for standalone")
    endif()

    if (DEFINED SA_HOSTED_CLAP_NAME)
        set(hasclapname TRUE)
    endif()
    target_compile_definitions(${SA_TARGET} PRIVATE
            PLUGIN_ID="${SA_PLUGIN_ID}"
            PLUGIN_INDEX=${SA_PLUGIN_INDEX}
            $<$<BOOL:${SA_STATICALLY_LINKED_CLAP_ENTRY}>:STATICALLY_LINKED_CLAP_ENTRY=1>
            $<$<BOOL:${hasclapname}>:HOSTED_CLAP_NAME="${SA_HOSTED_CLAP_NAME}">
            OUTPUT_NAME="${SA_OUTPUT_NAME}"
            )

    target_link_libraries(${SA_TARGET} PRIVATE
            ${salib}
            )
endfunction(target_add_standalone_wrapper)
