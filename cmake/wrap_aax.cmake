function(private_add_aax_wrapper_sources)
    set(oneValueArgs TARGET)
    cmake_parse_arguments(PAAX "" "${oneValueArgs}" "" ${ARGN})

    set(tg ${PAAX_TARGET})
    set(sd ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${tg} PUBLIC CLAP_WRAPPER_BUILD_FOR_AAX=1)
    if(WIN32)
        target_sources(${tg} PRIVATE ${sd}/src/detail/os/windows.cpp)
    elseif (APPLE)
        target_sources(${tg} PRIVATE ${sd}/src/detail/os/macos.mm)
    elseif(UNIX)
        target_sources(${tg} PRIVATE ${sd}/src/detail/os/linux.cpp)
    endif()

    target_sources(${tg} PRIVATE
            ${sd}/src/wrapasaax.h
            ${sd}/src/wrapasaax.cpp
            ${sd}/src/detail/aax/process.h
            ${sd}/src/detail/aax/process.cpp
            ${sd}/src/detail/aax/plugview.h
            ${sd}/src/detail/aax/plugview.cpp
            )

    target_include_directories(${AAX_TARGET}-clap-wrapper-aax-lib PRIVATE "${sd}/include")
endfunction(private_add_aax_wrapper_sources)

# define libraries
function(target_add_aax_wrapper)
    set(oneValueArgs
            TARGET
            OUTPUT_NAME
            SUPPORTS_ALL_NOTE_EXPRESSIONS
            SINGLE_PLUGIN_TUID

            BUNDLE_IDENTIFIER
            BUNDLE_VERSION

            WINDOWS_FOLDER_AAX

            ASSET_OUTPUT_DIRECTORY

            MACOS_EMBEDDED_CLAP_LOCATION
            )
    cmake_parse_arguments(AAX "" "${oneValueArgs}" "" ${ARGN} )

    guarantee_aaxsdk()

    if (NOT DEFINED AAX_TARGET)
        message(FATAL_ERROR "clap-wrapper: target_add_aax_wrapper requires a target")
    endif()

    if (NOT DEFINED AAX_OUTPUT_NAME)
        message(FATAL_ERROR "clap-wrapper: target_add_aax_wrapper requires an output name")
    endif()

    message(STATUS "clap-wrapper: Adding AAX Wrapper to target ${AAX_TARGET} generating '${AAX_OUTPUT_NAME}.aaxplugin'")

    if (NOT DEFINED WINDOWS_FOLDER_AAX)
        set(WINDOWS_FOLDER_AAX FALSE)
    endif()


    string(MAKE_C_IDENTIFIER ${AAX_OUTPUT_NAME} outidentifier)

    target_sources(${AAX_TARGET}
            PRIVATE
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/${sd}/src/wrapasaax.h
            ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/${sd}/src/wrapasaax.cpp)

    # Define the AAX plugin name and include the sources directly.
    # We need to indivduate this target since it will be different
    # for different options

    # this creates a individually configured wrapper library for each target
    if (NOT TARGET ${AAX_TARGET}-clap-wrapper-aax-lib)
        add_library(${AAX_TARGET}-clap-wrapper-aax-lib STATIC)
        private_add_aax_wrapper_sources(TARGET ${AAX_TARGET}-clap-wrapper-aax-lib)

        target_link_libraries(${AAX_TARGET}-clap-wrapper-aax-lib PUBLIC clap base-sdk-aax)

        # clap-wrapper-extensions are PUBLIC, so a clap linking the library can access the clap-wrapper-extensions
        target_link_libraries(${AAX_TARGET}-clap-wrapper-aax-lib PUBLIC
                clap-wrapper-compile-options-public
                clap-wrapper-extensions
                clap-wrapper-shared-detail)
        target_link_libraries(${AAX_TARGET}-clap-wrapper-aax-lib PRIVATE clap-wrapper-compile-options)

        target_compile_options(${AAX_TARGET}-clap-wrapper-aax-lib PRIVATE
                -DCLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS=$<IF:$<BOOL:${AAX_SUPPORTS_ALL_NOTE_EXPRESSIONS}>,1,0>
                )
    endif()

    set_target_properties(${AAX_TARGET} PROPERTIES LIBRARY_OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}")
    target_link_libraries(${AAX_TARGET} PUBLIC ${AAX_TARGET}-clap-wrapper-aax-lib )


    if (APPLE)
        if ("${AAX_BUNDLE_IDENTIFIER}" STREQUAL "")
            string(REPLACE "_" "-" repout ${outidentifier})
            set(AAX_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${repout}.aaxplugin")
        endif()

        if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
            set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
        endif()

        target_link_libraries (${AAX_TARGET} PUBLIC "-framework Foundation" "-framework CoreFoundation")
        set_target_properties(${AAX_TARGET} PROPERTIES
                BUNDLE True
                BUNDLE_EXTENSION aaxplugin
                LIBRARY_OUTPUT_NAME ${AAX_OUTPUT_NAME}
                MACOSX_BUNDLE_GUI_IDENTIFIER ${AAX_BUNDLE_IDENTIFIER}
                MACOSX_BUNDLE_BUNDLE_NAME ${AAX_OUTPUT_NAME}
                MACOSX_BUNDLE_BUNDLE_VERSION ${AAX_BUNDLE_VERSION}
                MACOSX_BUNDLE_SHORT_VERSION_STRING ${AAX_BUNDLE_VERSION}
                MACOSX_BUNDLE_INFO_PLIST ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/AAX_Info.plist.in
                )

        macos_include_clap_in_bundle(TARGET ${AAX_TARGET}
                MACOS_EMBEDDED_CLAP_LOCATION ${AAX_MACOS_EMBEDDED_CLAP_LOCATION})
        macos_bundle_flag(TARGET ${AAX_TARGET})

        if (NOT "${AAX_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
            set_target_properties(${AAX_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY ${AAX_ASSET_OUTPUT_DIRECTORY})
        endif()

    elseif(UNIX)
        target_link_libraries(${AAX_TARGET} PUBLIC "-ldl")
        target_link_libraries(${AAX_TARGET} PRIVATE "-Wl,--no-undefined")

        if ("${AAX_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
            set(aaxroot "${CMAKE_BINARY_DIR}")
            set(aaxroot_dor "$<IF:$<CONFIG:Debug>,Debug,Release>/")
            set(aaxroot_d "Debug/")
            set(aaxroot_r "Release/")
        else()
            set(aaxroot "${AAX_ASSET_OUTPUT_DIRECTORY}")
            set(aaxroot_dor "")
            set(aaxroot_d "")
            set(aaxroot_r "")
        endif()

        add_custom_command(TARGET ${AAX_TARGET} PRE_BUILD
                WORKING_DIRECTORY ${aaxroot}
                COMMAND ${CMAKE_COMMAND} -E make_directory "${aaxroot_dor}${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-linux"
                )
        set_target_properties(${AAX_TARGET} PROPERTIES
                LIBRARY_OUTPUT_NAME ${AAX_OUTPUT_NAME}
                LIBRARY_OUTPUT_DIRECTORY "${aaxroot}/${aaxroot_dor}${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-linux"
                LIBRARY_OUTPUT_DIRECTORY_DEBUG "${aaxroot}/${aaxroot_d}/${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-linux"
                LIBRARY_OUTPUT_DIRECTORY_RELEASE "${aaxroot}/${aaxroot_r}/${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-linux"
                SUFFIX ".so" PREFIX "")
    else()
        if (NOT ${WINDOWS_FOLDER_AAX})
            message(STATUS "clap-wrapper: Building AAX Single File")
            set_target_properties(${AAX_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_NAME ${AAX_OUTPUT_NAME}
                    SUFFIX ".aaxplugin")

            if (NOT "${AAX_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
                set_target_properties(${AAX_TARGET} PROPERTIES
                        LIBRARY_OUTPUT_DIRECTORY "${AAX_ASSET_OUTPUT_DIRECTORY}")
            endif()
        else()
            message(STATUS "clap-wrapper: Building AAX Bundle Folder")
            add_custom_command(TARGET ${AAX_TARGET} PRE_BUILD
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-win"
                    )
            set_target_properties(${AAX_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_NAME ${AAX_OUTPUT_NAME}
                    LIBRARY_OUTPUT_DIRECTORY "$<IF:$<CONFIG:Debug>,Debug,Release>/${CMAKE_BINARY_DIR}/${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-win"
                    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-win"
                    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${AAX_OUTPUT_NAME}.aaxplugin/Contents/x86_64-win"
                    SUFFIX ".aaxplugin")

            if (NOT "${AAX_ASSET_OUTPUT_DIRECTORY}" STREQUAL "")
                message(WARNING "AAX Custom Asset Output Dir and WINDOWS AAX folder bundle not yet implemented")
            endif()
        endif()
    endif()

    if (${CLAP_WRAPPER_COPY_AFTER_BUILD})
        target_copy_after_build(TARGET ${AAX_TARGET} FLAVOR aax)
    endif()
endfunction(target_add_aax_wrapper)
