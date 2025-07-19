# The initial version of 'clap first' cmake support in a simple wrapper. This is subject
# to some syntactic change as we move the wrapper ahead, but the semantics and actions are
# stable.
#
# For examples see `tests/clap-first-example` or `https://github.com/baconpaul/six-sines`
#



# make_clapfirst_plugins assembles plugins in all formats - CLAP, VST3, AUv2, Standalone etc
# from two assets you provide. The first is a static library which contains symbols for the
# functions clap_init, clap_deinit, and clap_getfactory, named whatever you want. This is
# where 99.99% of your development will be. The second is a single c++ file which generates
# a clap_entry structure with the three functions provided in your static library.
#
# make_clap_first then assembles a clap, vst3, auv2 etc by recompiling the clap entry
# tiny file for each plugin format and introducing it to the plugin protocl, either by
# direct export with the clap or by other methods with the other formats. This assembled
# wrapper then links to your static library for your functions.
#
# This means in practice you just write a clap, but write it as a static lib not a dynamic
# lib, introduce a small fragment of c++ to make an export, and voila, you have all the formats
# in self contained standalone assembled items.

function(make_clapfirst_plugins)
    set(oneValueArgs
            TARGET_NAME   # name of the output target. It will be postpended _clap, _vst3, _all
            IMPL_TARGET   # name of the target you set up with your static library

            OUTPUT_NAME   # output name of your CLAP, VST3, etc...

            ENTRY_SOURCE  # source file of the small cpp with the export directives and link to init

            BUNDLE_IDENTIFIER  # macos bundle identifier and builds
            BUNDLE_VERSION

            COPY_AFTER_BUILD # If true, on mac and lin the clap/vst3/etc... will install locally

            STANDALONE_MACOS_ICON # Icons for standalone
            STANDALONE_WINDOWS_ICON

            WINDOWS_FOLDER_VST3 # True for a filder, false for single file (default)

            ASSET_OUTPUT_DIRECTORY
            RESOURCE_DIRECTORY # Entire directory to be copied into a bundle/folder's resources

            AUV2_MANUFACTURER_NAME # The AUV2 info. If absent we will probe the CLAP for the
            AUV2_MANUFACTURER_CODE # auv2 extension
            AUV2_SUBTYPE_CODE
            AUV2_INSTRUMENT_TYPE
    )
    set(multiValueArgs
            PLUGIN_FORMATS   # A list of plugin formats, "CLAP" "VST3" "AUV2"
            STANDALONE_CONFIGURATIONS # standalone configuration. This is a list of target names and clap ids
    )
    cmake_parse_arguments(C1ST "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )

    if (DEFINED C1ST_COPY_AFTER_BUILD)
        set(CLAP_WRAPPER_COPY_AFTER_BUILD ${C1ST_COPY_AFTER_BUILD})
    endif()

    if (NOT DEFINED C1ST_TARGET_NAME)
        message(FATAL_ERROR "clap-wrapper: make_clapfirst_plugins requires TARGET_NAME")
    endif()

    if (NOT DEFINED C1ST_WINDOWS_FOLDER_VST3)
        set(C1ST_WINDOWS_FOLDER_VST3 FALSE)
    endif()

    if (NOT DEFINED C1ST_RESOURCE_DIRECTORY)
        set(C1ST_RESOURCE_DIRECTORY "")
    endif()

    if (NOT DEFINED C1ST_PLUGIN_FORMATS)
        message(STATUS "clap-wrapper: clapfrist no firmats given. Turning them all on")
        set(C1ST_PLUGIN_FORMATS CLAP VST3 AUV2)
    endif()

    if (EMSCRIPTEN)
        set(BUILD_CLAP -1)
        set(BUILD_VST3 -1)
        set(BUILD_AUV2 -1)
        list(FIND C1ST_PLUGIN_FORMATS "WCLAP" BUILD_WCLAP)
    else()
        list(FIND C1ST_PLUGIN_FORMATS "CLAP" BUILD_CLAP)
        list(FIND C1ST_PLUGIN_FORMATS "VST3" BUILD_VST3)
        list(FIND C1ST_PLUGIN_FORMATS "AUV2" BUILD_AUV2)
        set(BUILD_WCLAP -1)

        if (${BUILD_CLAP} EQUAL -1)
            message(FATAL_ERROR "You must build a clap when making clap-first")
        endif()
    endif()

    if (NOT DEFINED C1ST_IMPL_TARGET)
        message(FATAL_ERROR "clap-wrapper: make_clapfirst_plugins requires IMPL_TARGET")
    endif()

    if (NOT DEFINED C1ST_ENTRY_SOURCE)
        message(FATAL_ERROR "clap-wrapper: make_clapfirst_plugins requires ENTRY_SOURCE")
    endif()

    if (NOT DEFINED C1ST_OUTPUT_NAME)
        message(STATUS "clap-wrapper: Defaulting clap first output name to ${C1ST_TARGET_NAME}")
        set(C1ST_OUTPUT_NAME ${C1ST_TARGET_NAME})
    endif()

    if (NOT DEFINED C1ST_BUNDLE_IDENTIFIER)
        message(STATUS "clap-wrapper: Defaulting clap first bundle identifier to clapfirst.${C1ST_TARGET_NAME}")
        set(C1ST_BUNDLE_IDENTIFIER "clapfirst.${C1ST_TARGET_NAME}")
    endif()

    if (NOT DEFINED C1ST_BUNDLE_VERSION)
        message(STATUS "clap-wrapper: Defaulting clap first version to 0.01")
        set(C1ST_BUNDLE_VERSION "0.01")
    endif()

    set(ALL_TARGET ${C1ST_TARGET_NAME}_all)
    add_custom_target(${ALL_TARGET})

    if (${BUILD_CLAP} GREATER -1)
        message(STATUS "clap-wrapper: ClapFirst is making a CLAP")
        set(CLAP_TARGET ${C1ST_TARGET_NAME}_clap)
        add_library(${CLAP_TARGET} MODULE ${C1ST_ENTRY_SOURCE})
        target_link_libraries(${CLAP_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
        target_add_clap_configuration(TARGET ${CLAP_TARGET}
                OUTPUT_NAME ${C1ST_OUTPUT_NAME}
                BUNDLE_IDENTIFIER "${C1ST_BUNDLE_IDENTIFIER}.clap"
                BUNDLE_VERSION "${C1ST_BUNDLE_VERSION}"
                RESOURCE_DIRECTORY "${C1ST_RESOURCE_DIRECTORY}"
        )
        if (DEFINED C1ST_ASSET_OUTPUT_DIRECTORY)
            if (NOT WIN32)
                set_target_properties(${CLAP_TARGET} PROPERTIES
                        LIBRARY_OUTPUT_DIRECTORY ${C1ST_ASSET_OUTPUT_DIRECTORY})
            else ()
                set_target_properties(${CLAP_TARGET} PROPERTIES
                        LIBRARY_OUTPUT_DIRECTORY "${C1ST_ASSET_OUTPUT_DIRECTORY}/CLAP")
            endif()
        endif()
        set_target_properties(${CLAP_TARGET} PROPERTIES BUNDLE TRUE MACOSX_BUNDLE TRUE)

        add_dependencies(${ALL_TARGET} ${CLAP_TARGET})
    endif()

    if (${BUILD_VST3} GREATER -1)
        message(STATUS "clap-wrapper: ClapFirst is making a VST3")

        set(VST3_TARGET ${C1ST_TARGET_NAME}_vst3)
        add_library(${VST3_TARGET} MODULE)
        target_sources(${VST3_TARGET} PRIVATE ${C1ST_ENTRY_SOURCE})
        target_link_libraries(${VST3_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
        set(vod "")
        if (DEFINED C1ST_ASSET_OUTPUT_DIRECTORY)
            if (NOT WIN32)
                set(vod "${C1ST_ASSET_OUTPUT_DIRECTORY}")
            else ()
                set(vod "${C1ST_ASSET_OUTPUT_DIRECTORY}/VST3")
            endif()
        endif()
        target_add_vst3_wrapper(TARGET ${VST3_TARGET}
                OUTPUT_NAME "${C1ST_OUTPUT_NAME}"
                BUNDLE_IDENTIFIER "${C1ST_BUNDLE_IDENTIFER}.vst3"
                BUNDLE_VERSION "${C1ST_BUNDLE_VERSION}"
                ASSET_OUTPUT_DIRECTORY "${vod}"
                WINDOWS_FOLDER_VST3 ${C1ST_WINDOWS_FOLDER_VST3}
                RESOURCE_DIRECTORY "${C1ST_RESOURCE_DIRECTORY}"
        )

        add_dependencies(${ALL_TARGET} ${VST3_TARGET})
    endif()

    if (APPLE AND ${BUILD_AUV2} GREATER -1)
        message(STATUS "clap-wrapper: ClapFirst is making a AUv2")
        set(AUV2_TARGET ${C1ST_TARGET_NAME}_auv2)
        add_library(${AUV2_TARGET} MODULE)
        target_sources(${AUV2_TARGET} PRIVATE ${C1ST_ENTRY_SOURCE})
        target_link_libraries(${AUV2_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
        if (DEFINED C1ST_AUV2_MANUFACTURER_CODE)
            target_add_auv2_wrapper(
                    TARGET ${AUV2_TARGET}
                    OUTPUT_NAME "${C1ST_OUTPUT_NAME}"
                    BUNDLE_IDENTIFIER "${C1ST_BUNDLE_IDENTIFER}.auv2"
                    BUNDLE_VERSION "${C1ST_BUNDLE_VERSION}"
                    RESOURCE_DIRECTORY "${C1ST_RESOURCE_DIRECTORY}"

                    MANUFACTURER_NAME "${C1ST_AUV2_MANUFACTURER_NAME}"
                    MANUFACTURER_CODE "${C1ST_AUV2_MANUFACTURER_CODE}"
                    SUBTYPE_CODE "${C1ST_AUV2_SUBTYPE_CODE}"
                    INSTRUMENT_TYPE "${C1ST_AUV2_INSTRUMENT_TYPE}"
            )
        else()
            target_add_auv2_wrapper(
                    TARGET ${AUV2_TARGET}
                    OUTPUT_NAME "${C1ST_OUTPUT_NAME}"
                    BUNDLE_IDENTIFIER "${C1ST_BUNDLE_IDENTIFER}.auv2"
                    BUNDLE_VERSION "${C1ST_BUNDLE_VERSION}"
                    RESOURCE_DIRECTORY "${C1ST_RESOURCE_DIRECTORY}"

                    CLAP_TARGET_FOR_CONFIG "${CLAP_TARGET}"
            )
        endif()

        if (DEFINED C1ST_ASSET_OUTPUT_DIRECTORY)
            set_target_properties(${AUV2_TARGET} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY ${C1ST_ASSET_OUTPUT_DIRECTORY})
        endif()

        add_dependencies(${ALL_TARGET} ${AUV2_TARGET})
    endif()

    if (${BUILD_WCLAP} GREATER -1)
        message(STATUS "clap-wrapper: ClapFirst is making a WCLAP (with Emscripten)")
        set(WCLAP_TARGET ${C1ST_TARGET_NAME}_wclap)
        add_executable(${WCLAP_TARGET} ${C1ST_ENTRY_SOURCE})
        target_link_libraries(${WCLAP_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
        target_add_wclap_configuration(TARGET ${WCLAP_TARGET}
                OUTPUT_NAME ${C1ST_OUTPUT_NAME}
                RESOURCE_DIRECTORY "${C1ST_RESOURCE_DIRECTORY}"
        )
        if (DEFINED C1ST_ASSET_OUTPUT_DIRECTORY)
            # set the RUNTIME path because module.wasm are built as binaries (dynamic libraries for WASM aren't well-defined)
            if (NOT WIN32)
                set_target_properties(${WCLAP_TARGET} PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY ${C1ST_ASSET_OUTPUT_DIRECTORY}/${C1ST_OUTPUT_NAME}.wclap)
            else ()
                set_target_properties(${WCLAP_TARGET} PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY "${C1ST_ASSET_OUTPUT_DIRECTORY}/WCLAP/${C1ST_OUTPUT_NAME}.wclap")
            endif()
        endif()

        # Emscripten flags
        set_target_properties(${WCLAP_TARGET} PROPERTIES
                COMPILE_FLAGS "-msimd128" # TODO: add this to the static library as well?
                LINK_FLAGS    "-msimd128 -sSTANDALONE_WASM --no-entry -s EXPORTED_FUNCTIONS=_clap_entry,_malloc -s INITIAL_MEMORY=512kb -s ALLOW_MEMORY_GROWTH=1 -s ALLOW_TABLE_GROWTH=1 -s PURE_WASI --export-table"
        )

        add_dependencies(${ALL_TARGET} ${WCLAP_TARGET})
    endif()

    if (NOT EMSCRIPTEN AND DEFINED C1ST_STANDALONE_CONFIGURATIONS)
        list(LENGTH C1ST_STANDALONE_CONFIGURATIONS salen)
        math(EXPR NUMSA "${salen}/3 - 1")

        if (${NUMSA} GREATER -1)
            foreach(SA_INDEX RANGE 0 ${NUMSA})
                math(EXPR start "${SA_INDEX}*3")
                math(EXPR end "${SA_INDEX}*3 + 3")
                message(STATUS "${start} ${end} ${NUMSA} ${SA_INDEX}")
                list(SUBLIST C1ST_STANDALONE_CONFIGURATIONS ${start} ${end} thissa)

                message(STATUS "${thissa}")
                list(GET thissa 0 postfix)
                list(GET thissa 1 saname)
                list(GET thissa 2 clapid)

                set(SATARG "${C1ST_TARGET_NAME}_${postfix}")
                message(STATUS "clap-wrapper: clapfirst standalone generating ${SATARG} -> ${saname}")
                add_executable(${SATARG})
                target_sources(${SATARG} PRIVATE ${C1ST_ENTRY_SOURCE})
                target_link_libraries(${SATARG} PRIVATE ${C1ST_IMPL_TARGET})
                target_add_standalone_wrapper(TARGET ${SATARG}
                        OUTPUT_NAME "${saname}"
                        STATICALLY_LINKED_CLAP_ENTRY TRUE
                        PLUGIN_ID "${clapid}"
                        MACOS_ICON "${C1ST_STANDALONE_MACOS_ICON}"
                        WINDOWS_ICON "${C1ST_STANDALONE_WINDOWS_ICON}"
                        RESOURCE_DIRECTORY "${C1ST_RESOURCE_DIRECTORY}"
                )
                if (DEFINED C1ST_ASSET_OUTPUT_DIRECTORY)
                    if (NOT WIN32)
                        set_target_properties(${SATARG} PROPERTIES
                                RUNTIME_OUTPUT_DIRECTORY ${C1ST_ASSET_OUTPUT_DIRECTORY})
                    else()
                        set_target_properties(${SATARG} PROPERTIES
                                RUNTIME_OUTPUT_DIRECTORY "${C1ST_ASSET_OUTPUT_DIRECTORY}/Standalone-${SATARG}")
                    endif()
                endif()

                add_dependencies(${ALL_TARGET} ${SATARG})

            endforeach ()
        endif()
    endif()

endfunction(make_clapfirst_plugins)