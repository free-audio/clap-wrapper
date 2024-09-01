
function (guarantee_cpm)
    if (NOT DEFINED CPMAddPackage)
        message(STATUS "clap-wrapper: including CPM")
        include(${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/external/CPM.cmake)
    endif()
endfunction(guarantee_cpm)

if (APPLE)
    set(CLAP_WRAPPER_PLATFORM "MAC" CACHE STRING "Clap Wrapper Platform: MAC")
elseif(UNIX)
    set(CLAP_WRAPPER_PLATFORM "LIN" CACHE STRING "Clap Wrapper Platform: Linux")
elseif(WIN32)
    set(CLAP_WRAPPER_PLATFORM "WIN" CACHE STRING "Clap Wrapper Platform: Windows")
endif()


# Setup a filesystem replacement on apple
# deployment targets below 10.15 do not support std::filesystem
if (APPLE)
    add_library(macos_filesystem_support INTERFACE)
    if (${CMAKE_OSX_DEPLOYMENT_TARGET} VERSION_GREATER_EQUAL "10.15")
        message(STATUS "cmake-wrapper: using std::filesystem as macOS deployment it ${CMAKE_OSX_DEPLOYMENT_TARGET}; using std::filesystem")
        target_compile_definitions(macos_filesystem_support INTERFACE -DMACOS_USE_STD_FILESYSTEM)
    else()
        guarantee_cpm()
        message(STATUS "cmake-wrapper: Downloading Gulrak Filesystem as a macOS deployment ${CMAKE_OSX_DEPLOYMENT_TARGET} < 10.15 stub")
        CPMAddPackage("gh:gulrak/filesystem#v1.5.14")
        target_link_libraries(macos_filesystem_support INTERFACE ghc_filesystem)
        target_compile_definitions(macos_filesystem_support INTERFACE -DMACOS_USE_GHC_FILESYSTEM)
    endif()
endif()


# Compiler options for warnings, errors etc
add_library(clap-wrapper-compile-options INTERFACE)
add_library(clap-wrapper-compile-options-public INTERFACE)
target_link_libraries(clap-wrapper-compile-options INTERFACE clap-wrapper-compile-options-public)

# This is useful for debugging cmake link problems2
# target_compile_definitions(clap-wrapper-compile-options INTERFACE -DTHIS_BUILD_USED_CLAP_WRAPPER_COMPILE_OPTIONS=1)
# target_compile_definitions(clap-wrapper-compile-options-public INTERFACE -DTHIS_BUILD_USED_CLAP_WRAPPER_COMPILE_OPTIONS_PUBLIC=1)
add_library(clap-wrapper-sanitizer-options INTERFACE)

target_compile_options(clap-wrapper-compile-options INTERFACE -D${CLAP_WRAPPER_PLATFORM}=1 -DCLAP_WRAPPER_VERSION="${CLAP_WRAPPER_VERSION}")
if (APPLE)
    target_link_libraries(clap-wrapper-compile-options INTERFACE macos_filesystem_support)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(clap-wrapper-compile-options INTERFACE -Wall -Wextra -Wno-unused-parameter -Wpedantic)
    if (WIN32)
        # CLang cant do werror on linux thanks to vst3 sdk
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            target_compile_options(clap-wrapper-compile-options INTERFACE -Werror)
            # Darned VST3 confuses "windows" with "msvc"
            message(STATUS "clap-wrapper: Turning off some warnings for windows gcc")
            target_compile_options(clap-wrapper-compile-options INTERFACE
                    -Wno-expansion-to-defined
                    -Wno-unknown-pragmas)
        endif()
    else()
        message(STATUS "clap-wrapper: Warnings are Errors")
        target_compile_options(clap-wrapper-compile-options INTERFACE -Werror)
    endif()
    if (${CMAKE_CXX_STANDARD} GREATER_EQUAL 20)
        message(STATUS "clap-wrapper: Turning off char8_t c++20 changes")
        target_compile_options(clap-wrapper-compile-options-public INTERFACE -fno-char8_t)
    endif()
    if (${CLAP_WRAPPER_ENABLE_SANITIZER})
        message(STATUS "clap-wrapper: enabling sanitizer build")

        target_compile_options(clap-wrapper-sanitizer-options INTERFACE
                -fsanitize=address,undefined,float-divide-by-zero
                -fsanitize-address-use-after-return=always
                -fsanitize-address-use-after-scope
                )
        target_link_options(clap-wrapper-sanitizer-options INTERFACE
                -fsanitize=address,undefined,float-divide-by-zero
                -fsanitize-address-use-after-return=always
                -fsanitize-address-use-after-scope
                )
        target_link_libraries(clap-wrapper-compile-options INTERFACE clap-wrapper-sanitizer-options)
    endif()

endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(clap-wrapper-compile-options INTERFACE /utf-8)
    if (${CMAKE_CXX_STANDARD} GREATER_EQUAL 20)
        message(STATUS "clap-wrapper: Turning off char8_t c++20 changes")
        target_compile_options(clap-wrapper-compile-options-public INTERFACE /Zc:char8_t-)
    endif()
endif()


# A platform-specific way to copy after build
function(target_copy_after_build)
    set(oneValueArgs
            TARGET
            FLAVOR
            )
    cmake_parse_arguments(CAB "" "${oneValueArgs}" "" ${ARGN})

    if (${CAB_FLAVOR} STREQUAL "vst3")
        set(postfix "vst3")
        set(macdir "VST3")
        set(lindir ".vst3")
    elseif (${CAB_FLAVOR} STREQUAL "auv2")
        set(postfix "component")
        set(macdir "Components")
        set(lindir ".component")
    endif ()

    if (APPLE)
        message(STATUS "clap-wrapper: will copy ${CAB_TARGET} / ${CAB_FLAVOR} after build")
        set(input_bundle "$<TARGET_FILE_DIR:${CAB_TARGET}>/../../../$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}")
        add_custom_command(TARGET ${CAB_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "~/Library/Audio/Plug-Ins/${macdir}"
                COMMAND ${CMAKE_COMMAND} -E echo installing "${input_bundle}" "~/Library/Audio/Plug-Ins/${macdir}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${input_bundle}" "~/Library/Audio/Plug-Ins/${macdir}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}"
                )
    elseif (UNIX)
        message(STATUS "clap-wrapper: will copy ${CAB_TARGET} / ${CAB_FLAVOR} after build (untested)")
        set(products_folder "${CMAKE_BINARY_DIR}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_DIR>")
        add_custom_command(TARGET ${CAB_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "~/${lindir}"
                COMMAND ${CMAKE_COMMAND} -E echo installing "${products_folder}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}" "~/${lindir}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${products_folder}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}" "~/${lindir}/$<TARGET_PROPERTY:${CAB_TARGET},LIBRARY_OUTPUT_NAME>.${postfix}"
                )
    endif ()
endfunction(target_copy_after_build)

# Make sure the clap shared targes have been ejected
function(guarantee_clap_wrapper_shared)
    if (TARGET clap-wrapper-shared-detail)  # this has been called
        return()
    endif()

    # Define the extensions target
    add_library(clap-wrapper-extensions INTERFACE)
    target_include_directories(clap-wrapper-extensions INTERFACE include)

    add_library(clap-wrapper-shared-detail STATIC
            src/clap_proxy.h
            src/clap_proxy.cpp
            src/detail/shared/sha1.h
            src/detail/shared/sha1.cpp
            src/detail/clap/fsutil.h
            src/detail/clap/fsutil.cpp
            src/detail/clap/automation.h
            )
    target_link_libraries(clap-wrapper-shared-detail PUBLIC clap clap-wrapper-extensions clap-wrapper-compile-options)
    target_include_directories(clap-wrapper-shared-detail PUBLIC libs/fmt)
    target_include_directories(clap-wrapper-shared-detail PUBLIC src)

    if (APPLE)
        target_sources(clap-wrapper-shared-detail PRIVATE
                src/detail/clap/mac_helpers.mm
                )
        target_link_libraries(clap-wrapper-shared-detail PRIVATE macos_filesystem_support)
    endif()
endfunction(guarantee_clap_wrapper_shared)

# add a SetFile POST_BUILD for bundles if you aren't using xcode
function(macos_bundle_flag)
    set(oneValueArgs TARGET)
    cmake_parse_arguments(MBF "" "${oneValueArgs}" "" ${ARGN})

    if (NOT APPLE)
        message(WARNING "Calling macos_bundle_flag on non APPLE system. Is this intentional?")
        return()
    endif()

    if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
        add_custom_command(TARGET ${MBF_TARGET} POST_BUILD
            WORKING_DIRECTORY $<TARGET_PROPERTY:${MBF_TARGET},LIBRARY_OUTPUT_DIRECTORY>
            COMMAND ${CMAKE_COMMAND} -E copy
                ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/macBundlePkgInfo
                "$<TARGET_FILE_DIR:${MBF_TARGET}>/../PkgInfo")
    endif()
    set_target_properties(${MBF_TARGET}
            PROPERTIES
            MACOSX_BUNDLE TRUE
            XCODE_ATTRIBUTE_GENERATE_PKGINFO_FILE "YES"
            )
endfunction(macos_bundle_flag)

# MacOS can load from bundle/Contents/PlugIns. This allows you to stage a clap into that
# location optionally. Called by the target_add_xyz_wrapper functions to allow self contained
# non  relinked macos bundles
function(macos_include_clap_in_bundle)
    set(oneValueArgs TARGET MACOS_EMBEDDED_CLAP_LOCATION)
    cmake_parse_arguments(MBC "" "${oneValueArgs}" "" ${ARGN})

    if (NOT APPLE)
        message(WARNING "Calling macos_include_clap_in_bundle on non APPLE system. Is this intentional?")
        return()
    endif()

    if (NOT ${MBC_MACOS_EMBEDDED_CLAP_LOCATION} STREQUAL "")
        message(STATUS "clap-wraiier: including embedded clap in target ${MBC_TARGET}")
        add_custom_command(TARGET ${MBC_TARGET} PRE_BUILD
                WORKING_DIRECTORY $<TARGET_PROPERTY:${MBC_TARGET},LIBRARY_OUTPUT_DIRECTORY>
                COMMAND ${CMAKE_COMMAND} -E echo "Installing ${MBC_MACOS_EMBEDDED_CLAP_LOCATION} in $<TARGET_PROPERTY:${MBC_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${MBC_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns/$<TARGET_PROPERTY:${MBC_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.clap"
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_PROPERTY:${MBC_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${MBC_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns"
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${MBC_MACOS_EMBEDDED_CLAP_LOCATION} "$<TARGET_PROPERTY:${MBC_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${MBC_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns/$<TARGET_PROPERTY:${MBC_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.clap"
                )
    endif ()
endfunction(macos_include_clap_in_bundle)

if(${CMAKE_SIZEOF_VOID_P} EQUAL 4)
    # a 32 bit build is odd enough that it might be an error. Chirp.
    message(STATUS "clap-wrapper: configured as 32 bit build. Intentional?")
endif()
message(STATUS "clap-wrapper: source dir is ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}, platform is ${CLAP_WRAPPER_PLATFORM}")
