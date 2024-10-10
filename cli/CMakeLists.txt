add_executable(clap-wrapper-cli
        src/main.cpp
)

CPMAddPackage(NAME CL11
        GITHUB_REPOSITORY CLIUtils/CLI11
        GIT_TAG v2.4.2)

# TODO: deduplicate this with the vendored fmt -
#       for some reason, the cli won't link
#       if we include the vendored version
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY fmtlib/fmt
        GIT_TAG 11.0.2
        OPTIONS
        "FMT_INSTALL ON"
)

target_link_libraries(clap-wrapper-cli PUBLIC CLI11::CLI11 fmt::fmt)
