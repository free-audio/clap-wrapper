# This code is 'where we are going' but not 'where we are yet' way to make clap wrapped plugins
# easily. Right now we are developing it and it is subject to change. When this message is replaced
# with some basic documentation, you are probably safe, but until then, chat with baconpaul or timo
# on discord before you use it ok?

function(make_clapfirst_plugins)
    set(oneValueArgs
            TARGET_NAME
            IMPL_TARGET

            OUTPUT_NAME

            ENTRY_SOURCE

            BUNDLE_IDENTIFIER
            BUNDLE_VERSION

            COPY_AFTER_BUILD
    )
    set(multiValueArgs
            PLUGIN_FORMATS
            STANDALONE_CONFIGURATIONS
    )
    cmake_parse_arguments(C1ST "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )

    if (DEFINED C1ST_COPY_AFTER_BUILD)
        set(CLAP_WRAPPER_COPY_AFTER_BUILD ${C1ST_COPY_AFTER_BUILD})
    endif()

    if (NOT DEFINED C1ST_TARGET_NAME)
        message(FATAL_ERROR "clap-wrapper: make_clapfirst_plugins requires TARGET_NAME")
    endif()

    if (NOT DEFINED C1ST_PLUGIN_FORMATS)
        message(STATUS "clap-wrapper: clapfrist no firmats given. Turning them all on")
        set(C1ST_PLUGIN_FORMATS CLAP VST3 AUV2)
    endif()

    list(FIND C1ST_PLUGIN_FORMATS "CLAP" BUILD_CLAP)
    list(FIND C1ST_PLUGIN_FORMATS "VST3" BUILD_VST3)
    list(FIND C1ST_PLUGIN_FORMATS "AUV2" BUILD_AUV2)

    if (${BUILD_CLAP} EQUAL -1)
        message(FATAL_ERROR "You must build a clap when making clap-first")
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
        )
        set_target_properties(${CLAP_TARGET} PROPERTIES BUNDLE TRUE MACOSX_BUNDLE TRUE)

        add_dependencies(${ALL_TARGET} ${CLAP_TARGET})
    endif()

    if (${BUILD_VST3} GREATER -1)
        message(STATUS "clap-wrapper: ClapFirst is making a VST3")

        set(VST3_TARGET ${C1ST_TARGET_NAME}_vst3)
        add_library(${VST3_TARGET} MODULE)
        target_sources(${VST3_TARGET} PRIVATE ${C1ST_ENTRY_SOURCE})
        target_link_libraries(${VST3_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
        target_add_vst3_wrapper(TARGET ${VST3_TARGET}
                OUTPUT_NAME "${C1ST_OUTPUT_NAME}"
                BUNDLE_IDENTIFIER "${C1ST_BUNDLE_IDENTIFER}.auv2"
                BUNDLE_VERSION "${C1ST_BUNDLE_VERSION}"
        )

        add_dependencies(${ALL_TARGET} ${VST3_TARGET})
    endif()

    if (APPLE AND ${BUILD_AUV2} GREATER -1)
        message(STATUS "clap-wrapper: ClapFirst is making a AUv2")
        set(AUV2_TARGET ${C1ST_TARGET_NAME}_auv2)
        add_library(${AUV2_TARGET} MODULE)
        target_sources(${AUV2_TARGET} PRIVATE ${C1ST_ENTRY_SOURCE})
        target_link_libraries(${AUV2_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
        target_add_auv2_wrapper(
                TARGET ${AUV2_TARGET}
                OUTPUT_NAME "${C1ST_OUTPUT_NAME}"
                BUNDLE_IDENTIFIER "${C1ST_BUNDLE_IDENTIFER}.auv2"
                BUNDLE_VERSION "${C1ST_BUNDLE_VERSION}"

                CLAP_TARGET_FOR_CONFIG "${CLAP_TARGET}"
        )

        add_dependencies(${ALL_TARGET} ${AUV2_TARGET})
    endif()

    if (DEFINED C1ST_STANDALONE_CONFIGURATIONS)
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
                        PLUGIN_ID "${clapid}")

                add_dependencies(${ALL_TARGET} ${SATARG})

            endforeach ()
        endif()
    endif()

endfunction(make_clapfirst_plugins)