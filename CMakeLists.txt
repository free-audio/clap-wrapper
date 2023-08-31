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
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "Minimum macOS version")
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

if(${CMAKE_SIZEOF_VOID_P} EQUAL 4)
	# a 32 bit build is odd enough that it might be an error. Chirp.
	message(STATUS "clap-wrapper: configured as 32 bit build. Intentional?")
endif()

# discover the plugin paths and enable them
include(cmake/enable_sdks.cmake)
message(STATUS "clap-wrapper: executing at ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "clap-wrapper: platform is ${PLATFORM}")

# provide the CLAP_WRAPPER_OUTPUT_NAME to specify the matching plugin name. This defines two
# variables: pluginname which is a target and clapname which is the actual output result name
if((NOT CLAP_WRAPPER_OUTPUT_NAME ) OR (CLAP_WRAPPER_OUTPUT_NAME STREQUAL ""))
	set(pluginname "clapasvst3")
	set(clapname ${pluginname})
	message(WARNING "clap-wrapper: CLAP_WRAPPER_OUTPUT_NAME not set - continuing with a default name `clapasvst3`")
else()
	string(MAKE_C_IDENTIFIER ${CLAP_WRAPPER_OUTPUT_NAME} pluginname)
	set(clapname ${CLAP_WRAPPER_OUTPUT_NAME})
endif()

# Generate a VST3 target name (prepare for AU)
set(pluginname_vst3 ${pluginname}_as_vst3)
message(STATUS "clap-wrapper: VST3 plugin target is '${pluginname_vst3}' generating target '${clapname}.vst3'")


# Define the extensions target
add_library(clap-wrapper-extensions INTERFACE)
target_include_directories(clap-wrapper-extensions INTERFACE include)

# Define the VST3 plugin name and include the sources directly
add_library(${pluginname_vst3} MODULE ${wrappersources_vst3} ${vst3sources})
target_include_directories(${pluginname_vst3} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${pluginname_vst3} PRIVATE ${VST3_SDK_ROOT} ${VST3_SDK_ROOT}/public.sdk ${VST3_SDK_ROOT}/pluginterfaces)
target_compile_definitions(${pluginname_vst3} PRIVATE -D${PLATFORM}=1)
set_target_properties(${pluginname_vst3} PROPERTIES LIBRARY_OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}")

# Link to the clap libraryes
target_link_libraries(${pluginname_vst3} PRIVATE clap-core clap-wrapper-extensions)

# Support ome of the options at cmake time
target_compile_options(${pluginname_vst3} PRIVATE
		-DCLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS=$<IF:$<BOOL:${CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS}>,1,0>
		)

if (NOT "${CLAP_VST3_TUID_STRING}" STREQUAL "")
	message(STATUS "clap-wrapper: Using cmake-specified VST3 TUID ${CLAP_VST3_TUID_STRING}")
	target_compile_options(${pluginname_vst3} PRIVATE
			-DCLAP_VST3_TUID_STRING="${CLAP_VST3_TUID_STRING}"
			)
endif()

target_compile_options(${pluginname_vst3} PRIVATE $<IF:$<CONFIG:Debug>,-DDEVELOPMENT=1,-DRELEASE=1>) # work through steinbergs alternate choices for these

if (APPLE)
	if ("${CLAP_WRAPPER_BUNDLE_IDENTIFIER}" STREQUAL "")
		set(CLAP_WRAPPER_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${pluginname}.vst3")
	endif()

	if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
		set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
	endif()

	target_link_libraries (${pluginname_vst3} PUBLIC "-framework Foundation" "-framework CoreFoundation")
	set_target_properties(${pluginname_vst3} PROPERTIES
			BUNDLE True
			BUNDLE_EXTENSION vst3
			MACOSX_BUNDLE_GUI_IDENTIFIER ${CLAP_WRAPPER_BUNDLE_IDENTIFIER}
			MACOSX_BUNDLE_BUNDLE_NAME ${clapname}
			MACOSX_BUNDLE_BUNDLE_VERSION ${CLAP_WRAPPER_BUNDLE_VERSION}
			MACOSX_BUNDLE_SHORT_VERSION_STRING ${CLAP_WRAPPER_BUNDLE_VERSION}
			MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/VST3_Info.plist.in
			)
	if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
		add_custom_command(TARGET ${pluginname_vst3} POST_BUILD
				WORKING_DIRECTORY $<TARGET_PROPERTY:${pluginname_vst3},LIBRARY_OUTPUT_DIRECTORY>
				COMMAND SetFile -a B "$<TARGET_PROPERTY:${pluginname_vst3},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${pluginname_vst3},BUNDLE_EXTENSION>")
	endif()
elseif(UNIX)
	target_link_libraries(${pluginname_vst3} PUBLIC "-ldl")
	target_link_libraries(${pluginname_vst3} PRIVATE "-Wl,--no-undefined")

	add_custom_command(TARGET ${pluginname_vst3} PRE_BUILD
			WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
			COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${clapname}.vst3/Contents/x86_64-linux"
	)
	set_target_properties(${pluginname_vst3} PROPERTIES
			LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<IF:$<CONFIG:Debug>,Debug,Release>/${clapname}.vst3/Contents/x86_64-linux"
			LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${clapname}.vst3/Contents/x86_64-linux"
			LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${clapname}.vst3/Contents/x86_64-linux"
			SUFFIX ".so" PREFIX "")
else()
	if (${CLAP_WRAPPER_WINDOWS_SINGLE_FILE})
		message(STATUS "Building VST3 Single File")
		set_target_properties(${pluginname_vst3} PROPERTIES SUFFIX ".vst3")
	else()
		message(STATUS "Building VST3 Bundle Folder")
		add_custom_command(TARGET ${pluginname_vst3} PRE_BUILD
				WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
				COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${clapname}.vst3/Contents/x86_64-win"
				)
		set_target_properties(${pluginname_vst3} PROPERTIES
				LIBRARY_OUTPUT_DIRECTORY "$<IF:$<CONFIG:Debug>,Debug,Release>/${CMAKE_BINARY_DIR}/${clapname}.vst3/Contents/x86_64-win"
				LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${clapname}.vst3/Contents/x86_64-win"
				LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${clapname}.vst3/Contents/x86_64-win"
				SUFFIX ".vst3")
	endif()
endif()


