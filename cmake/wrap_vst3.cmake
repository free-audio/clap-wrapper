function(private_add_vst3_wrapper_sources)
    set(oneValueArgs TARGET)
    cmake_parse_arguments(PV3 "" "${oneValueArgs}" "" ${ARGN})

    set(tg ${PV3_TARGET})
    set(sd ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${tg} PUBLIC CLAP_WRAPPER_BUILD_FOR_VST3=1)
    if(WIN32)
        target_sources(${tg} PRIVATE ${sd}/src/detail/os/windows.cpp)
    elseif (APPLE)
        target_sources(${tg} PRIVATE ${sd}/src/detail/os/macos.mm)
    elseif(UNIX)
        target_sources(${tg} PRIVATE ${sd}/src/detail/os/linux.cpp)
    endif()

    target_sources(${tg} PRIVATE
            ${sd}/src/wrapasvst3.h
            ${sd}/src/wrapasvst3.cpp
            ${sd}/src/wrapasvst3_entry.cpp
            ${sd}/src/detail/ara/ara.h
            ${sd}/src/detail/vst3/parameter.h
            ${sd}/src/detail/vst3/parameter.cpp
            ${sd}/src/detail/vst3/plugview.h
            ${sd}/src/detail/vst3/plugview.cpp
            ${sd}/src/detail/vst3/state.h
            ${sd}/src/detail/vst3/process.h
            ${sd}/src/detail/vst3/process.cpp
            ${sd}/src/detail/vst3/categories.h
            ${sd}/src/detail/vst3/categories.cpp
            ${sd}/src/detail/vst3/aravst3.h
            )

    target_include_directories(${V3_TARGET}-clap-wrapper-vst3-lib PRIVATE "${sd}/include")
endfunction(private_add_vst3_wrapper_sources)

# define libraries
function(target_add_vst3_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            SUPPORTS_ALL_NOTE_EXPRESSIONS
            SINGLE_PLUGIN_TUID

            BUNDLE_IDENTIFIER
            BUNDLE_VERSION

            WINDOWS_FOLDER_VST3

            MACOS_EMBEDDED_CLAP_LOCATION
            )
    cmake_parse_arguments(V3 "" "${oneValueArgs}" "" ${ARGN} )

    guarantee_vst3sdk()

    if (NOT DEFINED V3_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_vst3_wrapper requires a target")
    endif()

    if (NOT DEFINED V3_OUTPUT_NAME)
        message(FATAL_ERROR "clap-wrapper: target_add_vst3_wrapper requires an output name")
    endif()

    message(STATUS "clap-wrapper: Adding VST3 Wrapper to target ${V3_TARGET} generating '${V3_OUTPUT_NAME}.vst3'")

    if (NOT DEFINED V3_WINDOWS_FOLDER_VST3)
        set(V3_WINDOWS_FOLDER_VST3 FALSE)
    endif()


    string(MAKE_C_IDENTIFIER ${V3_OUTPUT_NAME} outidentifier)

    target_sources(${V3_TARGET}
            PRIVATE
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/${sd}/src/wrapasvst3_export_entry.cpp)


    # Define the VST3 plugin name and include the sources directly.
    # We need to indivduate this target since it will be different
    # for different options

    # this creates a individually configured wrapper library for each target
    if (NOT TARGET ${V3_TARGET}-clap-wrapper-vst3-lib)
        add_library(${V3_TARGET}-clap-wrapper-vst3-lib STATIC)
        private_add_vst3_wrapper_sources(TARGET ${V3_TARGET}-clap-wrapper-vst3-lib)

        target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC clap base-sdk-vst3)

        # clap-wrapper-extensions are PUBLIC, so a clap linking the library can access the clap-wrapper-extensions
        target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC clap-wrapper-extensions clap-wrapper-shared-detail)
        target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PRIVATE clap-wrapper-compile-options)

        target_compile_options(${V3_TARGET}-clap-wrapper-vst3-lib PRIVATE
                -DCLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS=$<IF:$<BOOL:${V3_SUPPORTS_ALL_NOTE_EXPRESSIONS}>,1,0>
                )
    endif()


    if (NOT "${V3_SINGLE_PLUGIN_TUID}" STREQUAL "")
        message(STATUS "clap-wrapper: Using cmake-specified VST3 TUID ${V3_SINGLE_PLUGIN_TUID}")
        target_compile_options(${V3_TARGET}-clap-wrapper-vst3-lib  PRIVATE
                -DCLAP_VST3_TUID_STRING="${V3_SINGLE_PLUGIN_TUID}"
                )
    endif()
    set_target_properties(${V3_TARGET} PROPERTIES LIBRARY_OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}")
    target_link_libraries(${V3_TARGET} PUBLIC ${V3_TARGET}-clap-wrapper-vst3-lib )


    if (APPLE)
        if ("${V3_BUNDLE_IDENTIFIER}" STREQUAL "")
            string(REPLACE "_" "-" repout ${outidentifier})
            set(V3_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${repout}.vst3")
        endif()

        if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
            set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
        endif()

        target_link_libraries (${V3_TARGET} PUBLIC "-framework Foundation" "-framework CoreFoundation")
        set_target_properties(${V3_TARGET} PROPERTIES
                BUNDLE True
                BUNDLE_EXTENSION vst3
                LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                MACOSX_BUNDLE_GUI_IDENTIFIER ${V3_BUNDLE_IDENTIFIER}
                MACOSX_BUNDLE_BUNDLE_NAME ${V3_OUTPUT_NAME}
                MACOSX_BUNDLE_BUNDLE_VERSION ${V3_BUNDLE_VERSION}
                MACOSX_BUNDLE_SHORT_VERSION_STRING ${V3_BUNDLE_VERSION}
                MACOSX_BUNDLE_INFO_PLIST ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/VST3_Info.plist.in
                )

        macos_include_clap_in_bundle(TARGET ${V3_TARGET}
                MACOS_EMBEDDED_CLAP_LOCATION ${V3_MACOS_EMBEDDED_CLAP_LOCATION})
        macos_bundle_flag(TARGET ${V3_TARGET})
    elseif(UNIX)
        target_link_libraries(${V3_TARGET} PUBLIC "-ldl")
        target_link_libraries(${V3_TARGET} PRIVATE "-Wl,--no-undefined")

        add_custom_command(TARGET ${V3_TARGET} PRE_BUILD
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
                )
        set_target_properties(${V3_TARGET} PROPERTIES
                LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<IF:$<CONFIG:Debug>,Debug,Release>/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
                LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
                LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
                SUFFIX ".so" PREFIX "")
    else()
        if (NOT ${V3_WINDOWS_FOLDER_VST3})
            message(STATUS "clap-wrapper: Building VST3 Single File")
            set_target_properties(${V3_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                    SUFFIX ".vst3")
        else()
            message(STATUS "clap-wrapper: Building VST3 Bundle Folder")
            add_custom_command(TARGET ${V3_TARGET} PRE_BUILD
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
                    )
            set_target_properties(${V3_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                    LIBRARY_OUTPUT_DIRECTORY "$<IF:$<CONFIG:Debug>,Debug,Release>/${CMAKE_BINARY_DIR}/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
                    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
                    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
                    SUFFIX ".vst3")
        endif()
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${V3_TARGET} FLAVOR vst3)
    endif()
endfunction(target_add_vst3_wrapper)
