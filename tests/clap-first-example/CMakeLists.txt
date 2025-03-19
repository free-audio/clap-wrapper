# This is a test of our clap-first makefiles we use in CI, and as such serves as
# an example of doign this without helpers, with the absolute smalles cmake, and
# so forth. A more fulsome example, using C++, setting up compiler options for
# wider deployment, and so forth can be found at
#
# https://github.com/baconpaul/six-sines

project(clap-first-distortion)

set(PRODUCT_NAME "ClapFirst Bad Distortion")

# Step one is to create a static 'impl' library which contains a static
# implementation of clap_init, clap_deinit and clap_get_factor. Here those
# are all in one cpp file, but a common idiom is to have an -entry-impl file
# separate from the rest of your clap, especially in multi-plugin claps.
add_library(${PROJECT_NAME}-impl STATIC distortion_clap.cpp)
target_link_libraries(${PROJECT_NAME}-impl PUBLIC clap)

# Then we use make_clapfirst_plugin, provided by the wrappers, to create the
# project out of the impl and the cpp file which exports the entry
make_clapfirst_plugins(
        TARGET_NAME ${PROJECT_NAME}
        IMPL_TARGET ${PROJECT_NAME}-impl

        OUTPUT_NAME "${PRODUCT_NAME}"

        ENTRY_SOURCE "distortion_clap_entry.cpp"

        BUNDLE_IDENTIFER "org.free-audio.clap-first-bad-distortion"
        BUNDLE_VERSION ${PROJECT_VERSION}

        COPY_AFTER_BUILD FALSE

        PLUGIN_FORMATS CLAP VST3 AUV2 WCLAP

        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${PROJECT_NAME}_assets

        # You can set up the AUv2 for a single plugin here, or you can
        # set it up with the auv2 extension in your clap
        AUV2_MANUFACTURER_NAME "Free Audio"
        AUV2_MANUFACTURER_CODE "FrAD"
        AUV2_SUBTYPE_CODE "BdDt"
        AUV2_INSTRUMENT_TYPE "aufx"

        # You can add a target-per-standalone you want. Syntax here is
        #   target-postfix output-name clap-id
        # This allows you to make multiple standalones from a multi-plugin clap
        # In this case, the standalone has no UI and is an audio to audio effect
        # so it isn't very useful
        # STANDALONE_CONFIGURATIONS
        # standalone "${PRODUCT_NAME}" "org.free-audio.clap-first-bad-distortion"
)

