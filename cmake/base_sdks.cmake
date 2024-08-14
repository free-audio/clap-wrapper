# base_sdks.cmake provudes a collection of `guarantee_` functions which set up a particular
# SDK. Those functions have the important property that they can be called as many times as you
# want and only the first call will set up the SDK. Practically this means they start with
# if NOT TARGET base-sdk-foo return.
#
# But this means that we can call, say, guarantee_vst3sdk inside the target_add_wrapper_vst3
# and if you add two vst3 targets you don't get an error. But if you add none, then you never
# load the VST3 SDK. You can have an AU and Standalone only cmake file which doesn't load or
# need VST3_SDK_ROOT set. So that little 'if NOT' is a pretty important part of our lazy
# guarantee, especially as we add formats (like VST3 or AAX) where individuals may not be party
# to the license agreement but will still want to build other formats.
#
# THe pattern we take to find a library is
#
# 1. is FOO_SDK_ROOT set? If so assume it is correct
# 2. is CMAKE_WRAPPER_DOWNLOAD_DEPENDENCIES set? If so download from github
# 3. Otherwise search parent paths for the sdk
#
# Then assert a key file from the SDK exists and FATAL_ERROR if it is missing.
#
# THis allows you to set the DOWNLOAD flag but override it for one but not other
# dependencies.

# locates certain paths/files in specific locations
# used to locate the base paths for the different Plugin SDKs
function(search_for_sdk_source)
    set(oneValueArgs SDKDIR RESULT)
    cmake_parse_arguments(SEARCH "" "${oneValueArgs}" "" ${ARGN} )

    message(STATUS "clap-wrapper: searching for '${SEARCH_SDKDIR}' in \"${CMAKE_CURRENT_SOURCE_DIR}\"...")

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${SEARCH_SDKDIR}")
        set(RES "${CMAKE_CURRENT_SOURCE_DIR}/libs/${SEARCH_SDKDIR}")
        message(STATUS "clap-wrapper: '${SEARCH_SDKDIR}' detected in libs subdirectory")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SEARCH_SDKDIR}")
        set(RES "${CMAKE_CURRENT_SOURCE_DIR}/${SEARCH_SDKDIR}")
        message(STATUS "clap-wrapper: '${SEARCH_SDKDIR}' SDK detected in subdirectory")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${SEARCH_SDKDIR}")
        set(RES "${CMAKE_CURRENT_SOURCE_DIR}/../${SEARCH_SDKDIR}")
        message(STATUS "clap-wrapper: '${SEARCH_SDKDIR}' SDK detected in parent subdirectory")
    else()
        message(FATAL_ERROR "Unable to detect ${SEARCH_SDKDIR}! Have you set -D${SEARCH_RESULT}=/path/to/sdk or CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=TRUE")
    endif()

    cmake_path(CONVERT "${RES}" TO_CMAKE_PATH_LIST RES)

    message(STATUS "clap-wrapper: ${SEARCH_RESULT} at ${RES}")
    set("${SEARCH_RESULT}" "${RES}" PARENT_SCOPE)
endfunction(search_for_sdk_source)

# The general pattern for a guarntee_foo is
#
# if in download mode, download, cache the download root
# if not, look for FOO_SDK_ROOT through upward search or variables
# Check the signal file is there
# set up the base-sdk-foo library target through interfaces or
# building sources, as appropriate

function(guarantee_clap)
    if (TARGET clap)
        if (NOT TARGET base-sdk-clap)
            add_library(base-sdk-clap ALIAS clap)
        endif()
        return()
    endif()


    if (NOT "${CLAP_SDK_ROOT}" STREQUAL "")
        # Use the provided root
    elseif (${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
        guarantee_cpm()
        CPMAddPackage(
                NAME "clap"
                GITHUB_REPOSITORY "free-audio/clap"
                GIT_TAG "1.2.0"
                EXCLUDE_FROM_ALL TRUE
                DOWNLOAD_ONLY TRUE
                SOURCE_DIR cpm/clap
        )
        set(CLAP_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/clap")
    else()
        search_for_sdk_source(SDKDIR clap RESULT CLAP_SDK_ROOT)
    endif()

    cmake_path(CONVERT "${CLAP_SDK_ROOT}" TO_CMAKE_PATH_LIST CLAP_SDK_ROOT)
    if(NOT EXISTS "${CLAP_SDK_ROOT}/include/clap/clap.h")
        message(FATAL_ERROR "There is no CLAP SDK at ${CLAP_SDK_ROOT}. Please set CLAP_SDK_ROOT appropriately ")
    endif()

    message(STATUS "clap-wrapper: Configuring clap sdk")
    add_subdirectory(${CLAP_SDK_ROOT} base-sdk-clap EXCLUDE_FROM_ALL)
endfunction(guarantee_clap)

function(guarantee_vst3sdk)
    if (TARGET base-sdk-vst3)
        return()
    endif()

    if (NOT "${VST3_SDK_ROOT}" STREQUAL "")
        # Use the provided root
    elseif(${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
        guarantee_cpm()
        CPMAddPackage(
                NAME "vst3sdk"
                GITHUB_REPOSITORY "steinbergmedia/vst3sdk"
                GIT_TAG "v3.7.6_build_18"
                EXCLUDE_FROM_ALL TRUE
                DOWNLOAD_ONLY TRUE
                GIT_SUBMODULES base public.sdk pluginterfaces
                SOURCE_DIR cpm/vst3sdk
        )

        set(VST3_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/vst3sdk")
    else()
        search_for_sdk_source(SDKDIR vst3sdk RESULT VST3_SDK_ROOT)
    endif()

    cmake_path(CONVERT "${VST3_SDK_ROOT}" TO_CMAKE_PATH_LIST VST3_SDK_ROOT)
    if(NOT EXISTS "${VST3_SDK_ROOT}/public.sdk/source/main/pluginfactory.cpp")
        message(FATAL_ERROR "There is no VST3 SDK at ${VST3_SDK_ROOT}. Please set VST3_SDK_ROOT appropriately ")
    endif()

    file(STRINGS "${VST3_SDK_ROOT}/CMakeLists.txt" SDKVERSION REGEX "^\[ ]*VERSION .*")
    string(STRIP ${SDKVERSION} SDKVERSION)
    string(REPLACE "VERSION " "" SDKVERSION ${SDKVERSION})
    message(STATUS "clap-wrapper: VST3 version: ${SDKVERSION}; VST3 Root ${VST3_SDK_ROOT}")

    add_library(base-sdk-vst3 STATIC)
    file(GLOB VST3_GLOB
            ${VST3_SDK_ROOT}/base/source/*.cpp
            ${VST3_SDK_ROOT}/base/thread/source/*.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/common/*.cpp
            ${VST3_SDK_ROOT}/pluginterfaces/base/*.cpp
            )
    if (UNIX AND NOT APPLE AND ${SDKVERSION} VERSION_LESS 3.7.9)
        # Sigh - VST3 SDK before 3.7.9 ships with non-working timer code
        get_filename_component(full_path_test_cpp ${VST3_SDK_ROOT}/base/source/timer.cpp ABSOLUTE)
        list(REMOVE_ITEM VST3_GLOB "${full_path_test_cpp}")
    endif()

    target_sources(base-sdk-vst3 PRIVATE ${VST3_GLOB})

    if (APPLE)
        target_sources(base-sdk-vst3 PRIVATE ${VST3_SDK_ROOT}/public.sdk/source/main/macmain.cpp)
    elseif (UNIX)
        target_sources(base-sdk-vst3 PRIVATE ${VST3_SDK_ROOT}/public.sdk/source/main/linuxmain.cpp)
    else()
        target_sources(base-sdk-vst3 PRIVATE ${VST3_SDK_ROOT}/public.sdk/source/main/dllmain.cpp)
    endif()

    target_sources(base-sdk-vst3 PRIVATE
            ${VST3_GLOB}
            ${vst3platform}
            ${VST3_SDK_ROOT}/public.sdk/source/main/pluginfactory.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/main/moduleinit.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstnoteexpressiontypes.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstaudioeffect.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstcomponent.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstcomponentbase.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstbus.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/vstparameters.cpp
            ${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
            )

    target_include_directories(base-sdk-vst3 PUBLIC ${VST3_SDK_ROOT} ${VST3_SDK_ROOT}/public.sdk ${VST3_SDK_ROOT}/pluginterfaces)
    target_compile_options(base-sdk-vst3 PUBLIC $<IF:$<CONFIG:Debug>,-DDEVELOPMENT=1,-DRELEASE=1>) # work through steinbergs alternate choices for these
    target_link_libraries(base-sdk-vst3 PUBLIC clap-wrapper-sanitizer-options)
    # The VST3SDK uses sprintf, not snprintf, which macOS flags as deprecated
    # to move people to snprintf. Silence that warning on the VST3 build
    if (APPLE)
        target_compile_options(base-sdk-vst3 PUBLIC -Wno-deprecated-declarations)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # The VST3 SDK confuses lld and long long int in format statements in some situations it seems
        target_compile_options(base-sdk-vst3 PUBLIC -Wno-format)

        # The SDK also does things like `#warning DEPRECATED No Linux implementation
        #	assert (false && "DEPRECATED No Linux implementation");` for some methods which
        # generates a cpp warning. Since we won't fix this do
        target_compile_options(base-sdk-vst3 PUBLIC -Wno-cpp)
    endif()
endfunction(guarantee_vst3sdk)

function(guarantee_auv2sdk)
    if (TARGET base-sdk-auv2)
        return()
    endif()
    if (NOT APPLE)
        add_library(base-sdk-auv2 INTERFACE)
        return()
    endif()

    if (NOT "${AUDIOUNIT_SDK_ROOT}" STREQUAL "")
        # Use the provided root
    elseif (${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
        guarantee_cpm()
        CPMAddPackage(
                NAME "AudioUnitSDK"
                GITHUB_REPOSITORY "apple/AudioUnitSDK"
                GIT_TAG "AudioUnitSDK-1.1.0"
                EXCLUDE_FROM_ALL TRUE
                DOWNLOAD_ONLY TRUE
                SOURCE_DIR cpm/AudioUnitSDK
        )
        set(AUDIOUNIT_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/AudioUnitSDK")
    else()
        search_for_sdk_source(SDKDIR AudioUnitSDK RESULT AUDIOUNIT_SDK_ROOT)
    endif()

    cmake_path(CONVERT "${AUDIOUNIT_SDK_ROOT}" TO_CMAKE_PATH_LIST AUDIOUNIT_SDK_ROOT)
    if(NOT EXISTS "${AUDIOUNIT_SDK_ROOT}/src/AudioUnitSDK")
        message(FATAL_ERROR "There is no Audio Unit SDK at ${AUDIOUNIT_SDK_ROOT}. Please set AUDIOUNIT_SDK_ROOT appropriately ")
    endif()

    message(STATUS "clap-wrapper: configuring auv2 sdk from ${AUDIOUNIT_SDK_ROOT}")
    set(AUSDK_SRC ${AUDIOUNIT_SDK_ROOT}/src/AudioUnitSDK)
    add_library(base-sdk-auv2 STATIC ${AUSDK_SRC}/AUBase.cpp
            ${AUSDK_SRC}/AUBuffer.cpp
            ${AUSDK_SRC}/AUBufferAllocator.cpp
            ${AUSDK_SRC}/AUEffectBase.cpp
            ${AUSDK_SRC}/AUInputElement.cpp
            ${AUSDK_SRC}/AUMIDIBase.cpp
            ${AUSDK_SRC}/AUMIDIEffectBase.cpp
            ${AUSDK_SRC}/AUOutputElement.cpp
            ${AUSDK_SRC}/AUPlugInDispatch.cpp
            ${AUSDK_SRC}/AUScopeElement.cpp
            ${AUSDK_SRC}/ComponentBase.cpp
            ${AUSDK_SRC}/MusicDeviceBase.cpp
            )
    target_include_directories(base-sdk-auv2 PUBLIC ${AUDIOUNIT_SDK_ROOT}/include)
endfunction(guarantee_auv2sdk)

function(guarantee_rtaudio)
    if (TARGET base-sdk-rtaudio)
        return()
    endif()

    set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio-uninstall" CACHE STRING "Name of 'uninstall' build target")
    set(RTAUDIO_BUILD_STATIC_LIBS TRUE CACHE BOOL "static rdmidi")
    set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "don't eject test targets")
    # Until https://github.com/thestk/rtaudio/pull/413/files
    # is in a versioned RTAudio we have to do this to stop the
    # ctest targets emerging
    set(BUILD_TESTING OFF CACHE BOOL "don't eject test targets")


    if (APPLE)
        # If you brew install jack, rtaudio will find it but not link it properly
        set(RTAUDIO_API_JACK FALSE CACHE STRING "No jack by default on macos")
    endif()

    if (NOT "${RTAUDIO_SDK_ROOT}" STREQUAL "")
        # Use the provided root
    elseif (${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
        guarantee_cpm()
        CPMAddPackage(
                NAME "rtaudio"
                GITHUB_REPOSITORY "thestk/rtaudio"
                GIT_TAG "6.0.1"
                EXCLUDE_FROM_ALL TRUE
                SOURCE_DIR cpm/rtaudio
                DOWNLOAD_ONLY TRUE
        )
        set(RTAUDIO_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/rtaudio")
    else()
        search_for_sdk_source(SDKDIR rtaudio RESULT RTAUDIO_SDK_ROOT)
    endif()

    cmake_path(CONVERT "${RTAUDIO_SDK_ROOT}" TO_CMAKE_PATH_LIST RTAUDIO_SDK_ROOT)
    if(NOT EXISTS "${RTAUDIO_SDK_ROOT}/RtAudio.h")
        message(FATAL_ERROR "There is no rtaudio at ${RTAUDIO_SDK_ROOT}. Please set RTAUDIO_SDK_ROOT appropriately ")
    endif()

    add_subdirectory(${RTAUDIO_SDK_ROOT} base-sdk-rtaudio EXCLUDE_FROM_ALL)
    add_library(base-sdk-rtaudio INTERFACE)
    target_link_libraries(base-sdk-rtaudio INTERFACE rtaudio)
endfunction(guarantee_rtaudio)



function(guarantee_rtmidi)
    if (TARGET base-sdk-rtmidi)
        return()
    endif()

    if (APPLE OR WIN32)
        set(RTMIDI_API_JACK FALSE CACHE BOOL "skip jack")
    endif()

    set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi-uninstall" CACHE STRING "Name of 'uninstall' build target")
    set(RTMIDI_BUILD_STATIC_LIBS TRUE CACHE BOOL "static rdmidi")
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "don't eject test targets")

    if (NOT "${RTMIDI_SDK_ROOT}" STREQUAL "")
        # Use the provided root
    elseif (${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
        guarantee_cpm()
        CPMAddPackage(
                NAME "rtmidi"
                GITHUB_REPOSITORY "thestk/rtmidi"
                GIT_TAG "6.0.0"
                EXCLUDE_FROM_ALL TRUE
                SOURCE_DIR cpm/rtmidi
                DOWNLOAD_ONLY TRUE
        )
        set(RTMIDI_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/rtmidi")
    else()
        search_for_sdk_source(SDKDIR rtmidi RESULT RTMIDI_SDK_ROOT)
    endif()

    cmake_path(CONVERT "${RTMIDI_SDK_ROOT}" TO_CMAKE_PATH_LIST RTMIDI_SDK_ROOT)
    if(NOT EXISTS "${RTMIDI_SDK_ROOT}/RtMidi.h")
        message(FATAL_ERROR "There is no rtmidi at ${RTMIDI_SDK_ROOT}. Please set RTMIDI_SDK_ROOT appropriately ")
    endif()

    add_subdirectory(${RTMIDI_SDK_ROOT} base-sdk-rtmidi EXCLUDE_FROM_ALL)
    add_library(base-sdk-rtmidi INTERFACE)
    target_link_libraries(base-sdk-rtmidi INTERFACE rtmidi)
endfunction(guarantee_rtmidi)

function(guarantee_wil)
    if (TARGET base-sdk-wil)
        return()
    endif()

    if (NOT "${WIL_SDK_ROOT}" STREQUAL "")
        # Use the provided root
    elseif (${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
        guarantee_cpm()
        CPMAddPackage(
                NAME "wil"
                GITHUB_REPOSITORY "microsoft/wil"
                GIT_TAG "v1.0.240803.1"
                EXCLUDE_FROM_ALL TRUE
                DOWNLOAD_ONLY TRUE
                SOURCE_DIR cpm/wil
        )
        set(WIL_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/wil")
    else()
        search_for_sdk_source(SDKDIR wil RESULT WIL_SDK_ROOT)
    endif()

    cmake_path(CONVERT "${WIL_SDK_ROOT}" TO_CMAKE_PATH_LIST WIL_SDK_ROOT)
    if(NOT EXISTS "${WIL_SDK_ROOT}/include/wil/common.h")
        message(FATAL_ERROR "There is no wil at ${WIL_SDK_ROOT}. Please set WIL_SDK_ROOT appropriately ")
    endif()

    add_library(base-sdk-wil INTERFACE)
    target_include_directories(base-sdk-wil INTERFACE "${WIL_SDK_ROOT}/include")
endfunction(guarantee_wil)
