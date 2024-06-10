project(clap-first-distortion)

# By A "Clap First" plugin, we mean a plugin which is *just* implmented as
# CLAP and uses this wrapper to project into the various other platforms.
#
# One special form of clap first plugin does that by creating a
# the platform plugins with the entire clap library built in. While there's
# a few ways to do this the pattern which eemse to work best is
#
# 1. Write the etnire DSP, handling, etc... of your clap as a library without
#    the clap entry
# 2. Have a minimal clap entry c++ file which just exposes the entry points from
#    that static library
# 3. Make each of the clap, vst3, auv2 etc... link the static library from 1 but
#    recomile the entry from 2, so the resulting plugin has the entry point exposed.
#
# We show an example of that here with a slightly modified version of the
# basic c99 distortion plugin, here re-coded using a C++ compiler.

# So first make the actual plugin as a static library
add_library(${PROJECT_NAME}_base STATIC distortion_clap.cpp)
target_link_libraries(${PROJECT_NAME}_base PUBLIC clap)


# Now build and configure the CLAP.
add_library(${PROJECT_NAME}_clap MODULE
        distortion_clap_entry.cpp
)
target_link_libraries(${PROJECT_NAME}_clap ${PROJECT_NAME}_base)

if(APPLE)
    set_target_properties(${PROJECT_NAME}_clap PROPERTIES
            BUNDLE True
            BUNDLE_EXTENSION clap
            LIBRARY_OUTPUT_NAME ClapFirstDistortion
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER org.free-audio.clapfirst
            MACOSX_BUNDLE_BUNDLE_NAME ClapFirstDistortion
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1"
            XCODE_ATTRIBUTE_GENERATE_PKGINFO_FILE "YES"
    )
elseif(UNIX)
    set_target_properties(${PROJECT_NAME}_clap PROPERTIES OUTPUT_NAME ClapFirstDistortion SUFFIX ".clap" PREFIX "")

else()
    set_target_properties(${PROJECT_NAME}_clap
            PROPERTIES
            OUTPUT_NAME ClapFirstDistortion
            SUFFIX ".clap" PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY CLAP
    )
endif()

# Building a VST3 is now easy. Make a MODULE library which compiles the entry code
# and links the base library, then use the wraper cmake code to expose it as a VST3
set(VST3_TARGET ${PROJECT_NAME}_vst3)
add_library(${VST3_TARGET} MODULE)
target_sources(${VST3_TARGET} PRIVATE distortion_clap_entry.cpp)
target_add_vst3_wrapper(TARGET ${VST3_TARGET}
        OUTPUT_NAME "ClapFirstDistortion"
)
target_link_libraries(${VST3_TARGET} PRIVATE ${PROJECT_NAME}_base)


# And the same for the standalone
set(SA_TARGET ${PROJECT_NAME}_standalone)
add_executable(${SA_TARGET})
target_sources(${SA_TARGET} PRIVATE distortion_clap_entry.cpp)
target_link_libraries(${SA_TARGET} PRIVATE ${PROJECT_NAME}_base)
target_add_standalone_wrapper(TARGET ${SA_TARGET}
        OUTPUT_NAME "ClapFirstDistortion"
        STATICALLY_LINKED_CLAP_ENTRY True
        PLUGIN_ID "org.free-audio.clap-first-bad-distortion")

if (APPLE)
    # And the same for the AUV2
    set(AUV2_TARGET ${PROJECT_NAME}_auv2)
    add_library(${AUV2_TARGET} MODULE)
    target_sources(${AUV2_TARGET} PRIVATE distortion_clap_entry.cpp)
    target_link_libraries(${AUV2_TARGET} PRIVATE ${PROJECT_NAME}_base)
    target_add_auv2_wrapper(
            TARGET ${AUV2_TARGET}
            OUTPUT_NAME "ClapFirstDistortion"
            BUNDLE_IDENTIFIER "org.freeaudio.bad-clap-first"
            BUNDLE_VERSION "1"

            MANUFACTURER_NAME "FreeAudio Team"
            MANUFACTURER_CODE "FrAU"
            SUBTYPE_CODE "clDi"
            INSTRUMENT_TYPE "aufx"

            CLAP_TARGET_FOR_CONFIG ${PROJECT_NAME}_clap
    )
endif()


# FInally collect those all in a utility target
add_custom_target(${PROJECT_NAME}_all_plugins)
add_dependencies(${PROJECT_NAME}_all_plugins ${PROJECT_NAME}_clap ${PROJECT_NAME}_vst3 ${PROJECT_NAME}_standalone)
if (APPLE)
    add_dependencies(${PROJECT_NAME}_all_plugins ${PROJECT_NAME}_auv2)
endif()