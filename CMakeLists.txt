#
#   clap-wrapper project
#
#   Copyright (c) 2022-2026 Timo Kaluza (defiantnerd)
#                           Paul Walker (baconpaul)
#
# There are a variety of advanced options to configure and build this wrapper, but
# for the simple case of a single plugin in a single clap, here are the relevant
# cmake-time options which this wrapper will use. See the wiki
# (https://github.com/free-audio/clap-wrapper/wiki) for a more complete description.
#
# SDK locations
# -------------
# Each SDK is resolved in this order:
#   1. an explicitly set <FOO>_SDK_ROOT cache variable
#   2. if CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES is ON, a CPM download from github
#   3. a search for the SDK directory in ./libs/<name>, ./<name> and ../<name>
#
# CLAP_SDK_ROOT       The location of the CLAP SDK (directory name 'clap')
# VST3_SDK_ROOT       The location of the VST3 SDK (directory name 'vst3sdk')
# AUDIOUNIT_SDK_ROOT  The location of Apple's AudioUnitSDK (directory name
#                     'AudioUnitSDK'); only needed for AUv2 builds. AUv3 uses
#                     system frameworks only and needs no extra SDK.
# AAX_SDK_ROOT        The location of the AAX SDK (directory name 'aax');
#                     only needed for AAX builds
# RTAUDIO_SDK_ROOT    The location of RtAudio (directory name 'rtaudio');
#                     only needed for standalone builds
# RTMIDI_SDK_ROOT     The location of RtMidi (directory name 'rtmidi');
#                     only needed for standalone builds
# WIL_SDK_ROOT        The location of the Windows Implementation Library
#                     (directory name 'wil'); only needed for standalone
#                     builds on Windows
#
# Which formats are built
# -----------------------
# When this project is the top level, a VST3 wrapper is always built, and AAX
# is built whenever the toolchain supports it (macOS, or Windows with MSVC on
# x64). The other formats are opt-in:
#
# CLAP_WRAPPER_BUILD_AUV2        Build the AUv2 wrapper (.component); requires the AudioUnitSDK
# CLAP_WRAPPER_BUILD_AUV3        Build the AUv3 app extension (.appex) plus its standalone
#                                host app; requires the Xcode generator (-G Xcode)
# CLAP_WRAPPER_BUILD_STANDALONE  Build a simple standalone executable hosting the CLAP
# CLAP_WRAPPER_BUILD_TESTS       Build the test CLAP wrappers
#
# Naming and packaging
# --------------------
# CLAP_WRAPPER_OUTPUT_NAME        The name of the resulting .vst3/.component/.appex/executable;
#                                 also the name of the .clap the wrapper loads. Defaults to
#                                 'clapasvst3' (with a warning)
# CLAP_WRAPPER_BUNDLE_IDENTIFIER  The macOS/iOS bundle identifier. Defaults to
#                                 'org.cleveraudio.wrapper.(name)(.suffix)'; AUv3 appends
#                                 '.auv3' (appex) and '.auv3standalone' (host app)
# CLAP_WRAPPER_BUNDLE_VERSION     The macOS/iOS bundle version. Defaults to 1.0
# CLAP_WRAPPER_WINDOWS_SINGLE_FILE  If TRUE (default) the windows .vst3 is a single file;
#                                 if FALSE, a VST3 3.7-spec folder bundle
#
# Behavior and build environment
# ------------------------------
# CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS  Forward all VST3 note expressions to your CLAP
#                                 without asking it via the wrapper extension
# CLAP_VST3_TUID_STRING           The VST3 component ::iid as a string; absent this, the
#                                 wrapper hashes the clap id
# CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES  Download the needed SDKs at configure time using
#                                 CPM from github
# CLAP_WRAPPER_COPY_AFTER_BUILD   After building, copy the plugin into the user's
#                                 plugin directory (~/Library/Audio/Plug-Ins/... on
#                                 macOS, ~/.vst3 on Linux)
# CLAP_WRAPPER_CXX_STANDARD       The C++ standard to compile with. Defaults to 17


cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0149 NEW)
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
	message(STATUS "[clap-wrapper]: OSX_DEPLOYMENT_TARGET is undefined. Setting to 10.13")
	set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13 CACHE STRING "Minimum macOS version")
endif()
if (NOT DEFINED CMAKE_OSX_ARCHITECTURES)
	message(STATUS "[clap-wrapper]: CMAKE_OSX_ARCHITECTURES is not set. Using native build for clap wrapper")
endif()
if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  if (WIN32)
    message(STATUS "CMAKE_MSVC_RUNTIME_LIBRARY not defined. Setting to static link")
  endif()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Statically link the MSVC Runtime
endif()
set(CMAKE_COLOR_DIAGNOSTICS ON)

# If your clap supports note expressions you *can* implement the wrapper extension here or you
# can just build with this turned on and it will forward all note expressions to your CLAP
option(CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS "Does the underlying CLAP support note expressions" OFF)
option(CLAP_WRAPPER_WINDOWS_SINGLE_FILE "Build a single fine (rather than folder) on windows" ON)
option(CLAP_WRAPPER_BUILD_TESTS "Build test CLAP wrappers" OFF)

project(clap-wrapper
	LANGUAGES C CXX
	VERSION 0.15.1
	DESCRIPTION "CLAP-as-X wrappers"
)
set(CLAP_WRAPPER_VERSION "${PROJECT_VERSION}" CACHE STRING "Version of the wrapper project")
message(STATUS "clap-wrapper: CLAP_WRAPPER_VERSION is ${CLAP_WRAPPER_VERSION}")

if (APPLE)
	enable_language(OBJC)
	enable_language(OBJCXX)
endif()

if (PROJECT_IS_TOP_LEVEL)
	if (DEFINED CLAP_WRAPPER_CXX_STANDARD)
		set(CMAKE_CXX_STANDARD ${CLAP_WRAPPER_CXX_STANDARD})
	else()
		if (NOT DEFINED CMAKE_CXX_STANDARD)
			set(CMAKE_CXX_STANDARD 17)
			if(MSVC)
			  add_compile_options(/std:c++17 /Zc:__cplusplus)
			endif()
		elseif ( ${CMAKE_CXX_STANDARD} VERSION_LESS 17)
			message(WARNING "CMAKE_CXX_STANDARD of ${CMAKE_CXX_STANDARD} < 17 not well tested")
		endif()
	endif()
endif()
message(STATUS "clap-wrapper: Building with C++ standard ${CMAKE_CXX_STANDARD}")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# discover the plugin paths and enable them. The library
# defines functions relative to this, so needs a cache
# variable with the source dir
set(CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "Clap Wrapper Source Location")
include(cmake/wrapper_functions.cmake)
include(cmake/top_level_default.cmake)

if (${CLAP_WRAPPER_BUILD_TESTS})
    add_subdirectory(tests)
endif()
