# The standalone's audio and MIDI run on the platform's own apis on macOS (CoreAudio,
# CoreMIDI) and Windows (WASAPI, WinMM). This switch takes RtAudio and RtMidi for both in
# their place, as every standalone did before. Linux has PipeWire and JACK for audio, each
# loaded at run time and built where its headers are found, and RtMidi for MIDI; without
# either header it takes RtAudio, as this switch does. Windows built with RtAudio's ASIO
# (RTAUDIO_API_ASIO) takes ASIO from RtAudio next to the native WASAPI either way.
option(CLAP_WRAPPER_STANDALONE_RTAUDIO
       "Standalone: RtAudio and RtMidi in place of CoreAudio/CoreMIDI, WASAPI/WinMM and PipeWire/JACK"
       OFF)

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
    cmake_parse_arguments(SA "" "${oneValueArgs}" "" ${ARGN} )

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

    # RtMidi with RtAudio, and on linux in any case
    set(sa_rtmidi ${sa_rtaudio})
    if (NOT APPLE AND NOT WIN32)
        set(sa_rtmidi ON)
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

    if (sa_rtaudio OR sa_asio)
        guarantee_rtaudio()
        target_sources(${salib} PRIVATE ${sasrc}/rtaudio_backend.cpp)
        target_link_libraries(${salib} PUBLIC base-sdk-rtaudio)
    endif()
    if (sa_rtaudio)
        target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_RTAUDIO=1)
    endif()
    if (sa_rtmidi)
        if ((sa_pipewire OR sa_jack) AND NOT TARGET base-sdk-rtmidi)
            # rtmidi's jack would link libjack, which the standalone loads at run time
            set(RTMIDI_API_JACK FALSE CACHE BOOL "RtMidi without JACK: the standalone loads libjack at run time" FORCE)
        endif()
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
    if (sa_pipewire OR sa_jack)
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
                MACOSX_BUNDLE_INFO_PLIST ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/Info.plist.in
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
        target_sources(${salib} PRIVATE ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/x11_gui.cpp)

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
