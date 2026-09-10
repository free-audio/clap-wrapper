
# The Linux standalone GUI is X11, which is also how it appears under XWayland.
# Turning this off builds a standalone with no window at all - audio, MIDI, the
# command line and plugin timers all still work - and needs no X11 development
# files present.
option(CLAP_WRAPPER_STANDALONE_X11_GUI "Build the X11 GUI for the Linux standalone" ON)

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

    guarantee_rtaudio()
    guarantee_rtmidi()

    set(salib ${SA_TARGET}-clap-wrapper-standalone-lib)
    add_library(${salib} STATIC
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/entry.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_host.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_host_audio.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_host_midi.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_settings.cpp
            )
    target_link_libraries(${salib}
            PUBLIC
            clap-wrapper-shared-detail
            base-sdk-rtmidi
            base-sdk-rtaudio
            )
    target_link_libraries(${salib} PRIVATE clap-wrapper-compile-options)

    if (APPLE)
        target_sources(${salib} PRIVATE)
        target_link_libraries(${salib}
                PUBLIC "-framework AVFoundation" "-framework Foundation" "-framework CoreFoundation" "-framework AppKit")

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
        target_link_libraries(${SA_TARGET} PRIVATE base-sdk-wil ComCtl32.Lib)

    elseif(UNIX)
        target_sources(${SA_TARGET} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone.cpp)

        # Not the GUI: error reporting and orderly shutdown, needed with or
        # without X11
        find_package(Threads REQUIRED)
        target_link_libraries(${salib} PUBLIC Threads::Threads)
        target_sources(${salib} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/linux_frontend.cpp
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/linux_command_line.cpp)

        if (CLAP_WRAPPER_STANDALONE_X11_GUI)
            # Rather than linking a bare 'X11' and letting a missing libx11-dev
            # turn up as a raw linker error
            find_package(X11)
            if (NOT X11_FOUND)
                message(FATAL_ERROR "clap-wrapper: the standalone X11 GUI needs the X11 development "
                        "files, which were not found. Install them (libx11-dev on debian/ubuntu, "
                        "libX11-devel on fedora, libx11 on arch) or configure with "
                        "-DCLAP_WRAPPER_STANDALONE_X11_GUI=OFF for a standalone with no window.")
            endif()

            message(STATUS "clap-wrapper: Using Standalone X11 gui for CLAP Wrapper")
            target_link_libraries(${salib} PUBLIC X11::X11)
            target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_STANDALONE_X11)
            target_sources(${salib} PRIVATE ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/x11_gui.cpp)
        else()
            message(STATUS "clap-wrapper: Standalone X11 gui disabled; the standalone will run without a window")
        endif()

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
