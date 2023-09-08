# wrapper components
# for VST3
#
# 2022 defiant nerd
#
# options
# set(CLAP_WRAPPER_OUTPUT_NAME "" CACHE STRING "The output name of the dynamically wrapped plugin")

# make CPM available
include(cmake/CPM.cmake)

if (${CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES})
	message(STATUS "clap-wrapper: Downloading dependencies using CPM")

	CPMAddPackage(
			NAME "vst3sdk"
			GITHUB_REPOSITORY "steinbergmedia/vst3sdk"
			GIT_TAG "v3.7.6_build_18"
			EXCLUDE_FROM_ALL TRUE
			DOWNLOAD_ONLY TRUE
			SOURCE_DIR cpm/vst3sdk
	)
	set(VST3_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/vst3sdk" CACHE STRING "Path to downloaded VST3SDK")
	message(STATUS "clap-wrapper: configuring vst3sdk in ${VST3_SDK_ROOT}")

	if(NOT TARGET clap)
		CPMAddPackage(
				NAME "clap"
				GITHUB_REPOSITORY "free-audio/clap"
				GIT_TAG "1.1.8"
				EXCLUDE_FROM_ALL TRUE
				DOWNLOAD_ONLY TRUE
				SOURCE_DIR cpm/clap
		)
		set(CLAP_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/clap" CACHE STRING "Path to downloaded CLAP SDK")
		message(STATUS "clap-wrapper: configuring clap in ${CLAP_SDK_ROOT}")
	endif()

	if (APPLE)
		if (${CLAP_WRAPPER_BUILD_AUV2})
			CPMAddPackage(
					NAME "AudioUnitSDK"
					GITHUB_REPOSITORY "apple/AudioUnitSDK"
					GIT_TAG "AudioUnitSDK-1.1.0"
					EXCLUDE_FROM_ALL TRUE
					DOWNLOAD_ONLY TRUE
					SOURCE_DIR cpm/AudioUnitSDK
			)
			set(AUDIOUNIT_SDK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cpm/AudioUnitSDK" CACHE STRING "Path to downloaded AUV2 SDK")
			message(STATUS "clap-wrapper: configuring AudioUnitSDK in ${AUDIOUNIT_SDK_ROOT}")
		endif()
	endif()
else()
	set(CLAP_SDK_ROOT "" CACHE STRING "Path to CLAP SDK")
	set(VST3_SDK_ROOT "" CACHE STRING "Path to VST3 SDK")
	set(AUDIOUNIT_SDK_ROOT "" CACHE STRING "Path to AUv2 SDK")
endif()

# locates certain paths/files in specific locations
# used to locate the base paths for the different Plugin SDKs 
function(LibrarySearchPath)
	set(oneValueArgs SDKDIR RESULT)
	cmake_parse_arguments(SEARCH "" "${oneValueArgs}" "" ${ARGN} )

	set(RES "")

	message(STATUS "clap-wrapper: searching for '${SEARCH_SDKDIR}' in \"${CMAKE_CURRENT_SOURCE_DIR}\"...")

	if ( RES STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${SEARCH_SDKDIR}")
		set(RES "${CMAKE_CURRENT_SOURCE_DIR}/libs/${SEARCH_SDKDIR}")
		message(STATUS "clap-wrapper: '${SEARCH_SDKDIR}' detected in libs subdirectory")
	endif()

	if ( RES STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SEARCH_SDKDIR}")
		set(RES "${CMAKE_CURRENT_SOURCE_DIR}/${SEARCH_SDKDIR}")
		message(STATUS "clap-wrapper: '${SEARCH_SDKDIR}' SDK detected in subdirectory")
	endif()

	if ( RES STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${SEARCH_SDKDIR}")
		set(RES "${CMAKE_CURRENT_SOURCE_DIR}/../${SEARCH_SDKDIR}")
		message(STATUS "clap-wrapper: '${SEARCH_SDKDIR}' SDK detected in parent subdirectory")
	endif()

	if(RES STREQUAL "")
		message(FATAL_ERROR "Unable to detect ${SEARCH_SDKDIR}! Have you set -D${SEARCH_RESULT}=/path/to/sdk or CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=TRUE")
	endif()

	cmake_path(CONVERT "${RES}" TO_CMAKE_PATH_LIST RES)

	message(STATUS "clap-wrapper: ${SEARCH_RESULT} at ${RES}")
	set("${SEARCH_RESULT}" "${RES}" PARENT_SCOPE)
endfunction(LibrarySearchPath)

function(DetectCLAP)
  if(CLAP_SDK_ROOT STREQUAL "")
	LibrarySearchPath(SDKDIR clap RESULT CLAP_SDK_ROOT)
  endif()

  cmake_path(CONVERT "${CLAP_SDK_ROOT}" TO_CMAKE_PATH_LIST CLAP_SDK_ROOT)
  # message(STATUS "clap-wrapper: CLAP SDK location: ${CLAP_SDK_ROOT}")
  set(CLAP_SDK_ROOT "${CLAP_SDK_ROOT}" PARENT_SCOPE)
endfunction(DetectCLAP)

# allow the programmer to place the VST3 SDK at various places
function(DetectVST3SDK)
  if(VST3_SDK_ROOT STREQUAL "")
	  LibrarySearchPath(SDKDIR vst3sdk RESULT VST3_SDK_ROOT)
  endif()

  cmake_path(CONVERT "${VST3_SDK_ROOT}" TO_CMAKE_PATH_LIST VST3_SDK_ROOT)
  # message(STATUS "clap-wrapper: VST3 SDK location: ${VST3_SDK_ROOT}")
  set(VST3_SDK_ROOT "${VST3_SDK_ROOT}" PARENT_SCOPE)
endfunction()

function(DefineCLAPASVST3Sources)
	file(GLOB VST3_GLOB
			${VST3_SDK_ROOT}/base/source/*.cpp
			${VST3_SDK_ROOT}/base/thread/source/*.cpp
			${VST3_SDK_ROOT}/public.sdk/source/common/*.cpp
			${VST3_SDK_ROOT}/pluginterfaces/base/*.cpp
			)

	if (APPLE)
		set(vst3platform ${VST3_SDK_ROOT}/public.sdk/source/main/macmain.cpp)
	elseif (UNIX)
		set(vst3platform ${VST3_SDK_ROOT}/public.sdk/source/main/linuxmain.cpp)
	else()
		set(vst3platform ${VST3_SDK_ROOT}/public.sdk/source/main/dllmain.cpp)
	endif()

	set(vst3sources
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
			CACHE STRING "VST3 SDK Source List"
			)

	if( UNIX AND NOT APPLE )
		# Sigh - ${VST3_SDK_ROOT} ships with non-working code if you has it
		get_filename_component(full_path_test_cpp ${VST3_SDK_ROOT}/base/source/timer.cpp ABSOLUTE)
		list(REMOVE_ITEM vst3sources "${full_path_test_cpp}")
	endif()

	if(WIN32)
		set(os_wrappersources src/detail/os/windows.cpp)
	endif()

	if (APPLE)
		set(os_wrappersources
			src/detail/clap/mac_helpers.mm
			src/detail/os/macos.mm
		)
	endif()

	if(UNIX AND NOT APPLE)
	set(os_wrappersources
		src/detail/os/linux.cpp
	)
	endif()

	set(wrappersources_vst3
		src/clap_proxy.h
		src/clap_proxy.cpp
		src/wrapasvst3.h
		src/wrapasvst3.cpp
		src/wrapasvst3_entry.cpp
		src/detail/vst3/parameter.h
		src/detail/vst3/parameter.cpp
		src/detail/vst3/plugview.h
		src/detail/vst3/plugview.cpp
		src/detail/vst3/state.h
		src/detail/vst3/process.h
		src/detail/vst3/process.cpp
		src/detail/vst3/categories.h
		src/detail/vst3/categories.cpp
		src/detail/sha1.h
		src/detail/sha1.cpp
		src/detail/clap/fsutil.h
		src/detail/clap/fsutil.cpp
		src/detail/os/osutil.h
		src/detail/clap/automation.h
		${os_wrappersources}
			CACHE STRING "Clap Wrapper Library Sources")

	set(wrappersources_vst3_entry
		${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasvst3_export_entry.cpp
		CACHE STRING "Clap Wrapper Entry Point Sources")

endfunction(DefineCLAPASVST3Sources)

##################### 

# if target clap is already existent as target, there is no need to do this here
if (NOT TARGET clap)
	DetectCLAP()

	if(NOT EXISTS "${CLAP_SDK_ROOT}/include/clap/clap.h")
		message(FATAL_ERROR "There is no CLAP SDK at ${CLAP_SDK_ROOT}. Please set CLAP_SDK_ROOT appropriately ")
	endif()

	message(STATUS "clap-wrapper: Configuring CLAP SDK")
	add_subdirectory(${CLAP_SDK_ROOT} clap EXCLUDE_FROM_ALL)
endif()


DetectVST3SDK()

if(NOT EXISTS "${VST3_SDK_ROOT}/public.sdk")
    message(FATAL_ERROR "There is no VST3 SDK at ${VST3_SDK_ROOT} ")
endif()

message(STATUS "clap-wrapper: assimilating VST3 SDK")
file(STRINGS "${VST3_SDK_ROOT}/CMakeLists.txt" SDKVERSION REGEX "^\[ ]*VERSION .*")
string(STRIP ${SDKVERSION} SDKVERSION)
string(REPLACE "VERSION " "" SDKVERSION ${SDKVERSION})
message(STATUS "clap-wrapper: VST3 version: ${SDKVERSION}; VST3 Root ${VST3_SDK_ROOT}")


DefineCLAPASVST3Sources() 

#####################

# define platforms

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
		message(STATUS "cmake-wrapper: Downloading Gulrak Filesystem as a macOS deployment ${CMAKE_OSX_DEPLOYMENT_TARGET} < 10.15 stub")
		CPMAddPackage("gh:gulrak/filesystem#v1.5.14")
		target_link_libraries(macos_filesystem_support INTERFACE ghc_filesystem)
		target_compile_definitions(macos_filesystem_support INTERFACE -DMACOS_USE_GHC_FILESYSTEM)
	endif()
endif()

# define libraries
function(target_add_vst3_wrapper)
	set(oneValueArgs
			TARGET
			OUTPUT_NAME
			SUPPORTS_ALL_NOTE_EXPRESSIONS
			SINGLE_PLUGIN_TUID

			BUNDLE_IDENTIFIER
			BUNDLE_VERSION

			WINDOWS_FOLDER_VST3

			MACOS_EMBEDDED_CLAP_LOCATION
			)
	cmake_parse_arguments(V3 "" "${oneValueArgs}" "" ${ARGN} )

	if (NOT DEFINED V3_TARGET)
		message(FATAL_ERROR "clap-wrapper: target_add_vst3_wrapper requires a target")
	endif()

	if (NOT DEFINED V3_OUTPUT_NAME)
		message(FATAL_ERROR "clap-wrapper: target_add_vst3_wrapper requires an output name")
	endif()

	message(STATUS "clap-wrapper: Adding VST3 Wrapper to target ${V3_TARGET} generating '${V3_OUTPUT_NAME}.vst3'")

	if (NOT DEFINED V3_WINDOWS_FOLDER_VST3)
		set(V3_WINDOWS_FOLDER_VST3 FALSE)
	endif()


	string(MAKE_C_IDENTIFIER ${V3_OUTPUT_NAME} outidentifier)

	target_sources(${V3_TARGET} PRIVATE ${wrappersources_vst3_entry})

	# the library "vst3-pluginbase-lib" contains all code from the VST3 SDK like the interface definitions
	# and base classes and will be statically linked. This code is shared between all VST3 plugins.
	# it only needs to be created once.
	if (NOT TARGET vst3-pluginbase-lib)
		message(STATUS "clap-wrapper: creating vst3 library with root ${VST3_SDK_ROOT}")
		add_library(vst3-pluginbase-lib STATIC ${vst3sources})
		target_include_directories(vst3-pluginbase-lib PUBLIC ${VST3_SDK_ROOT} ${VST3_SDK_ROOT}/public.sdk ${VST3_SDK_ROOT}/pluginterfaces)
		target_compile_options(vst3-pluginbase-lib PUBLIC $<IF:$<CONFIG:Debug>,-DDEVELOPMENT=1,-DRELEASE=1>) # work through steinbergs alternate choices for these
		# The VST3SDK uses sprintf, not snprintf, which macOS flags as deprecated
		# to move people to snprintf. Silence that warning on the VST3 build
		if (APPLE)
			target_compile_options(vst3-pluginbase-lib PUBLIC -Wno-deprecated-declarations)
		endif()
		if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
			# The VST3 SDK confuses lld and long long int in format statements in some situations it seems
			target_compile_options(vst3-pluginbase-lib PUBLIC -Wno-format)

			# The SDK also does things like `#warning DEPRECATED No Linux implementation
			#	assert (false && "DEPRECATED No Linux implementation");` for some methods which
			# generates a cpp warning. Since we won't fix this do
			target_compile_options(vst3-pluginbase-lib PUBLIC -Wno-cpp)
		endif()
	endif()


	# Define the VST3 plugin name and include the sources directly.
	# We need to indivduate this target since it will be different
	# for different options

	# this creates a individually configured wrapper library for each target
	if (NOT TARGET ${V3_TARGET}-clap-wrapper-vst3-lib)
		set(wsv3 ${wrappersources_vst3})
		list(TRANSFORM wsv3 PREPEND "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/")

		add_library(${V3_TARGET}-clap-wrapper-vst3-lib STATIC ${wsv3})
		target_include_directories(${V3_TARGET}-clap-wrapper-vst3-lib PRIVATE "${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/include")
		target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC clap vst3-pluginbase-lib)

		# clap-wrapper-extensions are PUBLIC, so a clap linking the library can access the clap-wrapper-extensions
		target_compile_definitions(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC -D${CLAP_WRAPPER_PLATFORM}=1)
		target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC clap-wrapper-extensions)

		target_compile_options(${V3_TARGET}-clap-wrapper-vst3-lib PRIVATE
				-DCLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS=$<IF:$<BOOL:${V3_SUPPORTS_ALL_NOTE_EXPRESSIONS}>,1,0>
				)

		if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
			target_compile_options(${V3_TARGET}-clap-wrapper-vst3-lib PRIVATE -Wall -Wextra -Wno-unused-parameter -Wpedantic -Werror)
		endif()
		if (APPLE)
			target_link_libraries(${V3_TARGET}-clap-wrapper-vst3-lib PUBLIC macos_filesystem_support)
		endif()
	endif()


	if (NOT "${V3_SINGLE_PLUGIN_TUID}" STREQUAL "")
		message(STATUS "clap-wrapper: Using cmake-specified VST3 TUID ${V3_SINGLE_PLUGIN_TUID}")
		target_compile_options(${V3_TARGET}-clap-wrapper-vst3-lib  PRIVATE
				-DCLAP_VST3_TUID_STRING="${V3_SINGLE_PLUGIN_TUID}"
				)
	endif()
	set_target_properties(${V3_TARGET} PROPERTIES LIBRARY_OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}")
	target_link_libraries(${V3_TARGET} PUBLIC ${V3_TARGET}-clap-wrapper-vst3-lib )


	if (APPLE)
		if ("${V3_BUNDLE_IDENTIFIER}" STREQUAL "")
			set(V3_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${outidentifier}.vst3")
		endif()

		if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
			set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
		endif()

		target_link_libraries (${V3_TARGET} PUBLIC "-framework Foundation" "-framework CoreFoundation")
		set_target_properties(${V3_TARGET} PROPERTIES
				BUNDLE True
				BUNDLE_EXTENSION vst3
				LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
				MACOSX_BUNDLE_GUI_IDENTIFIER ${V3_BUNDLE_IDENTIFIER}
				MACOSX_BUNDLE_BUNDLE_NAME ${V3_OUTPUT_NAME}
				MACOSX_BUNDLE_BUNDLE_VERSION ${V3_BUNDLE_VERSION}
				MACOSX_BUNDLE_SHORT_VERSION_STRING ${V3_BUNDLE_VERSION}
				MACOSX_BUNDLE_INFO_PLIST ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/VST3_Info.plist.in
				)
		if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
			add_custom_command(TARGET ${V3_TARGET} POST_BUILD
					WORKING_DIRECTORY $<TARGET_PROPERTY:${V3_TARGET},LIBRARY_OUTPUT_DIRECTORY>
					COMMAND SetFile -a B "$<TARGET_PROPERTY:${V3_TARGET},LIBRARY_OUTPUT_NAME>.$<TARGET_PROPERTY:${V3_TARGET},BUNDLE_EXTENSION>"
					)
		endif()

		if (NOT ${V3_MACOS_EMBEDDED_CLAP_LOCATION} STREQUAL "")
			add_custom_command(TARGET ${V3_TARGET} POST_BUILD
					WORKING_DIRECTORY $<TARGET_PROPERTY:${V3_TARGET},LIBRARY_OUTPUT_DIRECTORY>
					COMMAND ${CMAKE_COMMAND} -E echo "Installing ${V3_MACOS_EMBEDDED_CLAP_LOCATION} in $<TARGET_PROPERTY:${V3_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${V3_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns/$<TARGET_PROPERTY:${V3_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.clap"
					COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_PROPERTY:${V3_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${V3_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns"
					COMMAND ${CMAKE_COMMAND} -E copy_directory ${V3_MACOS_EMBEDDED_CLAP_LOCATION} "$<TARGET_PROPERTY:${V3_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${V3_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns/$<TARGET_PROPERTY:${V3_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.clap"
					)
		endif()
	elseif(UNIX)
		target_link_libraries(${V3_TARGET} PUBLIC "-ldl")
		target_link_libraries(${V3_TARGET} PRIVATE "-Wl,--no-undefined")

		add_custom_command(TARGET ${V3_TARGET} PRE_BUILD
				WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
				COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
				)
		set_target_properties(${V3_TARGET} PROPERTIES
				LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
				LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<IF:$<CONFIG:Debug>,Debug,Release>/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
				LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
				LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-linux"
				SUFFIX ".so" PREFIX "")
	else()
		if (NOT ${V3_WINDOWS_FOLDER_VST3})
			message(STATUS "clap-wrapper: Building VST3 Single File")
			set_target_properties(${V3_TARGET} PROPERTIES
					LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
					SUFFIX ".vst3")
		else()
			message(STATUS "clap-wrapper: Building VST3 Bundle Folder")
			add_custom_command(TARGET ${V3_TARGET} PRE_BUILD
					WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
					COMMAND ${CMAKE_COMMAND} -E make_directory "$<IF:$<CONFIG:Debug>,Debug,Release>/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
					)
			set_target_properties(${V3_TARGET} PROPERTIES
					LIBRARY_OUTPUT_NAME ${V3_OUTPUT_NAME}
					LIBRARY_OUTPUT_DIRECTORY "$<IF:$<CONFIG:Debug>,Debug,Release>/${CMAKE_BINARY_DIR}/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
					LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
					LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release/${V3_OUTPUT_NAME}.vst3/Contents/x86_64-win"
					SUFFIX ".vst3")
		endif()
	endif()
endfunction(target_add_vst3_wrapper)


if (APPLE)
	if (${CLAP_WRAPPER_BUILD_AUV2})
		if(AUDIOUNIT_SDK_ROOT STREQUAL "")
			LibrarySearchPath(SDKDIR AudioUnitSDK RESULT AUDIOUNIT_SDK_ROOT)
		endif()

		cmake_path(CONVERT "${AUDIOUNIT_SDK_ROOT}" TO_CMAKE_PATH_LIST AUDIOUNIT_SDK_ROOT)
		message(STATUS "clap-wrapper: AudioUnit SDK location: ${AUDIOUNIT_SDK_ROOT}")

		set(AUSDK_SRC ${AUDIOUNIT_SDK_ROOT}/src/AudioUnitSDK)
		if (NOT TARGET auv2_sdk)
			add_library(auv2_sdk STATIC ${AUSDK_SRC}/AUBase.cpp
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
			target_include_directories(auv2_sdk PUBLIC ${AUDIOUNIT_SDK_ROOT}/include)
		endif()

		function(target_add_auv2_wrapper)
			set(oneValueArgs
					TARGET
					OUTPUT_NAME
					BUNDLE_IDENTIFIER
					BUNDLE_VERSION

					MANUFACTURER_NAME
					MANUFACTURER_CODE
					SUBTYPE_CODE
					INSTRUMENT_TYPE

					MACOS_EMBEDDED_CLAP_LOCATION
			)
			cmake_parse_arguments(AUV2 "" "${oneValueArgs}" "" ${ARGN} )

			if (NOT DEFINED AUV2_TARGET)
				message(FATAL_ERROR "clap-wrapper: target_add_auv2_wrapper requires a target")
			endif()

			if (NOT DEFINED AUV2_OUTPUT_NAME)
				message(FATAL_ERROR "clap-wrapper: target_add_auv2_wrapper requires an output name")
			endif()

			if (NOT DEFINED AUV2_MANUFACTURER_NAME)
				message(FATAL_ERROR "clap-wrapper: For now please specify AUV2 manufacturer name")
			endif()

			if (NOT DEFINED AUV2_MANUFACTURER_CODE)
				message(FATAL_ERROR "clap-wrapper: For now please specify AUV2 manufacturer code (4 chars)")
			endif()

			if (NOT DEFINED AUV2_SUBTYPE_CODE)
				message(FATAL_ERROR "clap-wrapper: For now please specify AUV2 subtype code (4 chars)")
			endif()

			if (NOT DEFINED AUV2_INSTRUMENT_TYPE)
				message(WARNING "clap-wrapper: auv2 instrument type not specified. Using aumu")
				set(AUV2_INSTRUMENT_TYPE "aumu")
			endif()

			set(AUV2_INSTRUMENT_TYPE ${AUV2_INSTRUMENT_TYPE} PARENT_SCOPE)
			set(AUV2_MANUFACTURER_NAME ${AUV2_MANUFACTURER_NAME} PARENT_SCOPE)
			set(AUV2_MANUFACTURER_CODE ${AUV2_MANUFACTURER_CODE} PARENT_SCOPE)
			set(AUV2_SUBTYPE_CODE ${AUV2_SUBTYPE_CODE} PARENT_SCOPE)

			message(STATUS "clap-wrapper: Adding AUV2 Wrapper to target ${AUV2_TARGET} generating '${AUV2_OUTPUT_NAME}.component'")

			string(MAKE_C_IDENTIFIER ${AUV2_OUTPUT_NAME} outidentifier)

			# This is a placeholder dummy until we actually write the AUv2
			# Similarly the subordinate library being an interface below
			# is a placeholder. When we write it we will follow a similar
			# split trick as for the vst3, mostly (but AUV2 is a bit different
			# with info.plist and entrypoint-per-instance stuff)
			target_sources(${AUV2_TARGET} PRIVATE ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/src/wrapasauv2.cpp)


			if (NOT TARGET ${AUV2_TARGET}-clap-wrapper-auv2-lib)
				# For now make this an interface
				add_library(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE )
				target_include_directories(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
				target_link_libraries(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE clap auv2_sdk)

				# clap-wrapper-extensions are PUBLIC, so a clap linking the library can access the clap-wrapper-extensions
				target_compile_definitions(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE -D${CLAP_WRAPPER_PLATFORM}=1)
				target_link_libraries(${AUV2_TARGET}-clap-wrapper-auv2-lib INTERFACE clap-wrapper-extensions macos_filesystem_support)

			endif()

			set_target_properties(${AUV2_TARGET} PROPERTIES LIBRARY_OUTPUT_NAME "${AUV2_OUTPUT_NAME}")
			target_link_libraries(${AUV2_TARGET} PUBLIC ${AUV2_TARGET}-clap-wrapper-auv2-lib)

			if ("${AUV2_BUNDLE_IDENTIFIER}" STREQUAL "")
				set(AUV2_BUNDLE_IDENTIFIER "org.cleveraudio.wrapper.${outidentifier}.auv2")
			endif()

			if ("${CLAP_WRAPPER_BUNDLE_VERSION}" STREQUAL "")
				set(CLAP_WRAPPER_BUNDLE_VERSION "1.0")
			endif()

			target_link_libraries (${AUV2_TARGET} PUBLIC
					"-framework Foundation"
					"-framework CoreFoundation"
					"-framework AudioToolbox")

			set_target_properties(${AUV2_TARGET} PROPERTIES
					BUNDLE True
					BUNDLE_EXTENSION component
					LIBRARY_OUTPUT_NAME ${AUV2_OUTPUT_NAME}
					MACOSX_BUNDLE_GUI_IDENTIFIER "${AUV2_BUNDLE_IDENTIFIER}.component"
					MACOSX_BUNDLE_BUNDLE_NAME ${AUV2_OUTPUT_NAME}
					MACOSX_BUNDLE_BUNDLE_VERSION ${AUV2_BUNDLE_VERSION}
					MACOSX_BUNDLE_SHORT_VERSION_STRING ${AUV2_BUNDLE_VERSION}
					MACOSX_BUNDLE_INFO_PLIST ${CLAP_WRAPPER_CMAKE_CURRENT_SOURCE_DIR}/cmake/auv2_Info.plist.in
					)
			if (NOT ${CMAKE_GENERATOR} STREQUAL "Xcode")
				add_custom_command(TARGET ${AUV2_TARGET} POST_BUILD
						WORKING_DIRECTORY $<TARGET_PROPERTY:${AUV2_TARGET},LIBRARY_OUTPUT_DIRECTORY>
						COMMAND SetFile -a B "$<TARGET_PROPERTY:${AUV2_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${AUV2_TARGET},BUNDLE_EXTENSION>")
			endif()

			if (NOT ${AUV2_MACOS_EMBEDDED_CLAP_LOCATION} STREQUAL "")
				add_custom_command(TARGET ${AUV2_TARGET} POST_BUILD
						WORKING_DIRECTORY $<TARGET_PROPERTY:${AUV2_TARGET},LIBRARY_OUTPUT_DIRECTORY>
						COMMAND ${CMAKE_COMMAND} -E echo "Installing ${AUV2_MACOS_EMBEDDED_CLAP_LOCATION} in $<TARGET_PROPERTY:${AUV2_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${AUV2_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns/$<TARGET_PROPERTY:${AUV2_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.clap"
						COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_PROPERTY:${AUV2_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${AUV2_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns"
						COMMAND ${CMAKE_COMMAND} -E copy_directory ${AUV2_MACOS_EMBEDDED_CLAP_LOCATION} "$<TARGET_PROPERTY:${AUV2_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.$<TARGET_PROPERTY:${AUV2_TARGET},BUNDLE_EXTENSION>/Contents/PlugIns/$<TARGET_PROPERTY:${AUV2_TARGET},MACOSX_BUNDLE_BUNDLE_NAME>.clap"
						)
			endif()

		endfunction(target_add_auv2_wrapper)
	endif()
endif()


# Define the extensions target
if ( NOT TARGET clap-wrapper-extensions)
	add_library(clap-wrapper-extensions INTERFACE)
	target_include_directories(clap-wrapper-extensions INTERFACE include)
endif()

if(${CMAKE_SIZEOF_VOID_P} EQUAL 4)
	# a 32 bit build is odd enough that it might be an error. Chirp.
	message(STATUS "clap-wrapper: configured as 32 bit build. Intentional?")
endif()
message(STATUS "clap-wrapper: source dir is ${CMAKE_CURRENT_SOURCE_DIR}, platform is ${CLAP_WRAPPER_PLATFORM}")

