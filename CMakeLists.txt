#
#	clap-wrapper project
#
#   Copyright (c) 2022-2024 Timo Kaluza (defiantnerd)
#                           Paul Walker (baconpaul)
#
# There are a variety of advanced options to configure and build this wrapper, but
# for the simple case of single plugin in a single clap, here are some relevant
# cmake-time options which this wrapper will use. See the documentation for a more
# complete description
#
# CLAP_SDK_ROOT The location of the clap library. Defaults to ../clap
# VST3_SDK_ROOT The location of the VST3 sdk. Defaults tp ../vst3sdk
#
# CLAP_WRAPPER_OUTPUT_NAME   The name of the resulting .vst3 or .component
# CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS  The wrapper will forward all VST3 note expressions to your CLAP
# CLAP_VST3_TUID_STRING  The VST3 component ::iid as a string; absent this wrapper hashes clap id
# CLAP_WRAPPER_BUNDLE_IDENTIFIER the macOS Bundle Identifier. Absent this it is 'org.cleveraudio.wrapper.(name)'
# CLAP_WRAPPER_BUNDLE_VERSION the macOS Bundle Version. Defaults to 1.0
# CLAP_WRAPPER_WINDOWS_SINGLE_FILE if set to TRUE (default) the windows .vst3 is a single file; false a 3.7 spec folder
# CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES if set will download the needed SDKs using CPM from github
# CLAP_WRAPPER_COPY_AFTER_BUILD if included mac and lin will copy to ~/... (lin t/k)

cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0149 NEW)
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
	message(STATUS "[clap-wrapper]: OSX_DEPLOYEMNT_TARGET is undefined. Setting to 10.13")
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
	VERSION 0.12.1
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

add_subdirectory(cli)