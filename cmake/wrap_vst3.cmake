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

    # psl
    target_include_directories(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC "${sd}/libs/psl")

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
            RESOURCE_DIRECTORY

            ASSET_OUTPUT_DIRECTORY

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

    if (NOT DEFINED V3_RESOURCE_DIRECTORY)
        set(V3_RESOURCE_DIRECTORY "")
    endif()

    if (NOT DEFINED V3_WINDOWS_FOLDER_VST3)
        if(TCLP_RESOURCE_DIRECTORY STREQUAL "")
            set(V3_WINDOWS_FOLDER_VST3 FALSE)
        else()
            set(V3_WINDOWS_FOLDER_VST3 TRUE)
        endif()
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
        target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC
                clap-wrapper-compile-options-public
                clap-wrapper-extensions
                clap-wrapper-shared-detail)
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

        if ("${V3_MACOS_EMBEDDED_CLAP_LOCATION}" STREQUAL "")
            # Copy resource directory, if defined
            if(NOT V3_RESOURCE_DIRECTORY STREQUAL "")
                add_custom_command(TARGET ${V3_TARGET} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_directory "${V3_RESOURCE_DIRECTORY}" "$<TARGET_FILE_DIR:${V3_TARGET}>/../Resources"
                )
            endif()
        else()
            macos_include_clap_in_bundle(TARGET ${V3_TARGET}
                    MACOS_EMBEDDED_CLAP_LOCATION ${V3_MACOS_EMBEDDED_CLAP_LOCATION})
        endif()
        macos_bundle_flag(TARGET ${V3_TARGET})

        if (NOT "${V3_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
            set_target_properties(${VST3_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY ${V3_ASSET_OUTPUT_DIRECTORY})
        endif()

    elseif(UNIX)
        message(STATUS "clap-wrapper: Building the VST3 Bundle Folder using the CMAKE_SYSTEM_PROCESSOR variable: (${CMAKE_SYSTEM_PROCESSOR})")

        target_link_libraries(${V3_TARGET} PUBLIC "-ldl")
        target_link_libraries(${V3_TARGET} PRIVATE "-Wl,--no-undefined")

        if ("${V3_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
            set(v3root "${CMAKE_BINARY_DIR}")
            set(v3root_dor "$<IF:$<CONFIG:Debug>,Debug,Release>/")
            set(v3root_d "Debug/")
            set(v3root_r "Release/")
        else()
            set(v3root "${V3_ASSET_OUTPUT_DIRECTORY}")
            set(v3root_dor "")
            set(v3root_d "")
            set(v3root_r "")
        endif()

        add_custom_command(TARGET ${V3_TARGET} PRE_BUILD
                WORKING_DIRECTORY ${v3root}
                COMMAND ${CMAKE_COMMAND} -E make_directory "${v3root_dor}${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux"
                )
        set_target_properties(${V3_TARGET} PROPERTIES
                LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                LIBRARY_OUTPUT_DIRECTORY "${v3root}/${v3root_dor}${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux"
                LIBRARY_OUTPUT_DIRECTORY_DEBUG "${v3root}/${v3root_d}/${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux"
                LIBRARY_OUTPUT_DIRECTORY_RELEASE "${v3root}/${v3root_r}/${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux"
                SUFFIX ".so" PREFIX "")
        # Copy resource directory, if defined
        if(NOT TCLP_RESOURCE_DIRECTORY STREQUAL "")
            message(WARNING "RESOURCE_DIRECTORY defined, but not (yet) supported for Unix VST3s")
        endif()
    else()
        if (NOT ${V3_WINDOWS_FOLDER_VST3})
            message(STATUS "clap-wrapper: Building VST3 Single File")
            set_target_properties(${V3_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                    SUFFIX ".vst3")

            if (NOT "${V3_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
                set_target_properties(${VST3_TARGET} PROPERTIES
                        LIBRARY_OUTPUT_DIRECTORY "${V3_ASSET_OUTPUT_DIRECTORY}")
            endif()

            # Copy resource directory, if defined
            if(NOT TCLP_RESOURCE_DIRECTORY STREQUAL "")
                message(WARNING "RESOURCE_DIRECTORY set, but WINDOWS_FOLDER_VST3=FALSE")
            endif()
        else()
            message(STATUS "clap-wrapper: Building the VST3 Bundle Folder using the CMAKE_SYSTEM_PROCESSOR variable: (${CMAKE_SYSTEM_PROCESSOR})")

            # Check against the list of supported targets found here:
            # https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Format.html#for-the-windows-platform
            if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86"
                    AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
                    AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64ec"
                    AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"
                    AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64x")
                message(WARNING "clap-wrapper: The architecture (${CMAKE_SYSTEM_PROCESSOR}) is not officially suported by VST3. This may cause issues when loading the resulting plug-in")
            endif()

            if ("${V3_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
                set(v3root "${CMAKE_BINARY_DIR}")
                set(v3root_dor "$<IF:$<CONFIG:Debug>,Debug,Release>/")
                set(v3root_d "Debug/")
                set(v3root_r "Release/")
            else()
                set(v3root "${V3_ASSET_OUTPUT_DIRECTORY}")
                set(v3root_dor "")
                set(v3root_d "")
                set(v3root_r "")
            endif()

            add_custom_command(TARGET ${V3_TARGET} PRE_BUILD
                    WORKING_DIRECTORY ${v3root}
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${v3root_dor}${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-win"
                    )
            set_target_properties(${V3_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
                    LIBRARY_OUTPUT_DIRECTORY "${v3root}/${v3root_dor}${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-win"
                    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${v3root}/${v3root_d}/${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-win"
                    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${v3root}/${v3root_r}/${V3_OUTPUT_NAME}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-win"
                    SUFFIX ".vst3")

            # Copy resource directory, if defined
            if(NOT TCLP_RESOURCE_DIRECTORY STREQUAL "")
                message(WARNING "RESOURCE_DIRECTORY defined, but not (yet) supported for Windows VST3s")
            endif()

        endif()
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${V3_TARGET} FLAVOR vst3)
    endif()
endfunction(target_add_vst3_wrapper)
