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
#

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.11 CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build Universal Always")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Statically link the MSVC Runtime
set(CMAKE_COLOR_DIAGNOSTICS ON)

# If your clap supports note expressions you *can* implement the wrapper extension here or you
# can just build with this turned on and it will forward all note expressions to your CLAP
option(CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS "Does the underlying CLAP support note expressions" OFF)
option(CLAP_WRAPPER_WINDOWS_SINGLE_FILE "Build a single fine (rather than folder) on windows" ON)

project(clap-wrapper
	LANGUAGES C CXX
	VERSION 0.0.4
	DESCRIPTION "CLAP-as-X wrappers"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# discover the plugin paths and enable them
include(cmake/enable_sdks.cmake)

# provide the CLAP_WRAPPER_OUTPUT_NAME to specify the matching plugin name. This defines two
# variables: pluginname which is a target and clapname which is the actual output result name
if((NOT CLAP_WRAPPER_OUTPUT_NAME ) OR (CLAP_WRAPPER_OUTPUT_NAME STREQUAL ""))
	set(pluginname "clapasvst3")
	message(WARNING "clap-wrapper: CLAP_WRAPPER_OUTPUT_NAME not set - continuing with a default name `clapasvst3`")
else()
	string(MAKE_C_IDENTIFIER ${CLAP_WRAPPER_OUTPUT_NAME} pluginname)
endif()

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

if (${CLAP_WRAPPER_BUILD_AUV2})
	add_library(${pluginname}_as_auv2 MODULE)
	target_add_auv2_wrapper(
			TARGET ${pluginname}_as_auv2
			OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}"
			BUNDLE_IDENTIFIER "${CLAP_WRAPPER_BUNDLE_IDENTIFIER}"
			BUNDLE_VERSION "${CLAP_WRAPPER_BUNDLE_VERSION}"

	)
endif()
