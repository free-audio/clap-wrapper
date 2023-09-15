
function(target_add_standalone_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            BUNDLE_IDENTIFIER
            BUNDLE_VERSION

            PLUGIN_INDEX
            PLUGIN_ID
            STATICALLY_LINKED_CLAP_ENTRY
            HOSTED_CLAP_NAME

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

    guarantee_rtaudio()
    guarantee_rtmidi()

    set(salib ${SA_TARGET}-clap-wrapper-standalone-lib)
    add_library(${salib} STATIC
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/entry.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_host.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_host_audio.cpp
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/standalone_host_midi.cpp
            )
    target_link_libraries(${salib}
            PUBLIC
            clap-wrapper-compile-options
            clap-wrapper-shared-detail
            base-sdk-rtmidi
            base-sdk-rtaudio
            )

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
                ${GEN_XIB}
                )

        set_target_properties(${SA_TARGET} PROPERTIES
                BUNDLE TRUE
                BUNDLE_NAME ${SA_OUTPUT_NAME}
                OUTPUT_NAME ${SA_OUTPUT_NAME}
                MACOSX_BUNDLE_BUNDLE_NAME ${SA_OUTPUT_NAME}
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_INFO_PLIST ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/macos/Info.plist.in
                RESOURCE "${GEN_XIB}"
                )

        if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
            message(STATUS "cmake-wrapper: ejecting xib->nib rules manually for ${CMAKE_GENERATOR} on ${SA_TARGET}")
            find_program(IBTOOL ibtool REQUIRED)
            add_custom_command(TARGET ${SA_TARGET} PRE_BUILD
                    COMMAND ${CMAKE_COMMAND} -E echo ${IBTOOL} --compile "$<TARGET_PROPERTY:${SA_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.app/Contents/Resources/MainMenu.nib" ${GEN_XIB}
                    COMMAND ${IBTOOL} --compile "$<TARGET_PROPERTY:${SA_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.app/Contents/Resources/MainMenu.nib" ${GEN_XIB}
                    )
        endif()

        macos_include_clap_in_bundle(TARGET ${SA_TARGET}
                MACOS_EMBEDDED_CLAP_LOCATION ${SA_MACOS_EMBEDDED_CLAP_LOCATION})

    elseif(UNIX)
        target_sources(${SA_TARGET} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone.cpp)

        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK gtkmm-3.0)
        if (${GTK_FOUND})
            message(STATUS "clap-wrapper: using gtkmm-3.0 for standalone")
            target_compile_options(${salib} PUBLIC ${GTK_CFLAGS})
            target_include_directories(${salib} PUBLIC ${GTK_INCLUDE_DIRS})
            target_link_libraries(${salib} PUBLIC ${GTK_LINK_LIBRARIES})
            target_compile_definitions(${salib} PUBLIC CLAP_WRAPPER_HAS_GTK3)
            target_sources(${salib} PRIVATE ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/detail/standalone/linux/gtkutils.cpp)
        else()
            message(STATUS "clap-wrapper: can't find gtkmm-3.0; no ui in standalone")
        endif()

    else()
        target_sources(${SA_TARGET} PRIVATE
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasstandalone.cpp)
    endif()

    if (DEFINED SA_HOSTED_CLAP_NAME)
        set(hasclapname TRUE)
    endif()
    target_compile_definitions(${SA_TARGET} PRIVATE
            PLUGIN_ID="${SA_PLUGIN_ID}"
            PLUGIN_INDEX=${SA_PLUGIN_INDEX}
            $<$<BOOL:${SA_STATICALLY_LINKED_CLAP_ENTRY}>:STATICALLY_LINKED_CLAP_ENTRY=1>
            $<$<BOOL:${hasclapname}>:HOSTED_CLAP_NAME="${SA_HOSTED_CLAP_NAME}">
            OUTPUT_NAME="${OUTPUT_NAME}"
            )

    target_link_libraries(${SA_TARGET} PRIVATE
            clap-wrapper-compile-options
            clap-wrapper-shared-detail
            ${salib}
            )
endfunction(target_add_standalone_wrapper)
