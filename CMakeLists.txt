#
#	clap-wrapper project
#
#   Copyright (c) 2022 Timo Kaluza (defiantnerd)
#					   Paul Walker (baconpaul)
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
# CLAP_WRAPPER_DONT_ADD_TARGETS if included in a CMakeList above skip adding targets
# CLAP_WRAPPER_COPY_AFTER_BUILD if included mac and lin will copy to ~/... (lin t/k)

cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13 CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build Universal Always")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Statically link the MSVC Runtime
set(CMAKE_COLOR_DIAGNOSTICS ON)

# If your clap supports note expressions you *can* implement the wrapper extension here or you
# can just build with this turned on and it will forward all note expressions to your CLAP
option(CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS "Does the underlying CLAP support note expressions" OFF)
option(CLAP_WRAPPER_WINDOWS_SINGLE_FILE "Build a single fine (rather than folder) on windows" ON)

project(clap-wrapper
	LANGUAGES C CXX
	VERSION 0.5.0
	DESCRIPTION "CLAP-as-X wrappers"
)

if (APPLE)
	enable_language(OBJC)
	enable_language(OBJCXX)
endif()

if (DEFINED CLAP_WRAPPER_CXX_STANDARD)
	set(CMAKE_CXX_STANDARD ${CLAP_WRAPPER_CXX_STANDARD})
else()
	set(CMAKE_CXX_STANDARD 17)
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
if (DEFINED CLAP_WRAPPER_CODE_CHECKS_ONLY)
	message(STATUS "clap-wrapper: code checks only; skipping wrapper functions")
else()
	include(cmake/wrapper_functions.cmake)
endif()

# automatically build the wrapper only if this is the top level project
set(skipbuild FALSE)
if (NOT PROJECT_IS_TOP_LEVEL)
	if(DEFINED CLAP_WRAPPER_DONT_ADD_TARGETS)
		set(skipbuild TRUE)
	endif()
endif()
if (DEFINED CLAP_WRAPPER_CODE_CHECKS_ONLY)
	set(skipbuild ${CLAP_WRAPPER_CODE_CHECKS_ONLY})
endif()

if (${skipbuild})
	message(STATUS "clap-wrapper: Skipping clap wrapper target ejection")
else()
	# provide the CLAP_WRAPPER_OUTPUT_NAME to specify the matching plugin name.
	if((NOT CLAP_WRAPPER_OUTPUT_NAME ) OR (CLAP_WRAPPER_OUTPUT_NAME STREQUAL ""))
		set(CLAP_WRAPPER_OUTPUT_NAME "clapasvst3")
		message(WARNING "clap-wrapper: CLAP_WRAPPER_OUTPUT_NAME not set - continuing with a default name `clapasvst3`")
	endif()

	string(MAKE_C_IDENTIFIER ${CLAP_WRAPPER_OUTPUT_NAME} pluginname)

	# Link the actual plugin library
	add_library(${pluginname}_as_vst3 MODULE)
	target_add_vst3_wrapper(
			TARGET ${pluginname}_as_vst3
			OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
			SUPPORTS_ALL_NOTE_EXPRESSIONS $<BOOL:${CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS}>
			SINGLE_PLUGIN_TUID "${CLAP_VST3_TUID_STRING}"
			BUNDLE_IDENTIFIER "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}"
			BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"
			)

	if (APPLE)
		if (${CLAP_WRAPPER_BUILD_AUV2})
			add_library(${pluginname}_as_auv2 MODULE)
			target_add_auv2_wrapper(
					TARGET ${pluginname}_as_auv2
					OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
					BUNDLE_IDENTIFIER "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}"
					BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"

					# AUv2 is still a work in progress. For now make this an
					# explict option to the single plugin version
					INSTRUMENT_TYPE "aumu"
					MANUFACTURER_NAME "cleveraudio.org"
					MANUFACTURER_CODE "clAd"
					SUBTYPE_CODE "gWrp"
			)
		endif()
	endif()

	if (${CLAP_WRAPPER_BUILD_STANDALONE})
		add_executable(${pluginname}_as_standalone)
		target_add_standalone_wrapper(TARGET ${pluginname}_as_standalone
		    OUTPUT_NAME ${CLAP_WRAPPER_OUTPUT_NAME}
		    HOSTED_CLAP_NAME ${CLAP_WRAPPER_OUTPUT_NAME}
		    PLUGIN_INDEX 0)
	endif()

endif()

