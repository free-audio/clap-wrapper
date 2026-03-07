#
# THis is *actually* a test. You almost definitely dont need to consult this directory
#


project(clap-macos-embedded)

set(PRODUCT_NAME "Clap Macos Embedded")

add_library(${PROJECT_NAME}_clap MODULE
        distortion_clap.cpp
        distortion_clap_entry.cpp
)

target_link_libraries(${PROJECT_NAME}_clap PRIVATE clap clap-wrapper-extensions)

target_add_clap_configuration(TARGET ${PROJECT_NAME}_clap
        OUTPUT_NAME ${PROJECT_NAME}
        BUNDLE_IDENTIFIER "${PROJECT_NAME}.clap"
        BUNDLE_VERSION "1.0"
)

add_library(${PROJECT_NAME}_auv2 MODULE)
add_dependencies(${PROJECT_NAME}_auv2 ${PROJECT_NAME}_clap)

target_add_auv2_wrapper(
        TARGET ${PROJECT_NAME}_auv2
        OUTPUT_NAME "${PROJECT_NAME}"
        BUNDLE_IDENTIFIER "${PROJECT_NAME}.auv2"
        BUNDLE_VERSION "1.0"

        # This allows CMake to override the auv2 extension configuration
        PREFER_CMAKE_AUV2_CONFIGURATION TRUE

        MANUFACTURER_NAME "Free Audio"
        MANUFACTURER_CODE "FRaD"
        SUBTYPE_CODE "qqsd"
        INSTRUMENT_TYPE "ffqq" # this is not a valid code, just here to test

        MACOS_EMBEDDED_CLAP_LOCATION "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}_clap>"
)