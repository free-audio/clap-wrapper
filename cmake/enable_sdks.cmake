# wrapper components
# for VST3
#
# 2022 defiant nerd
#
# options
# set(CLAP_WRAPPER_OUTPUT_NAME "" CACHE STRING "The output name of the dynamically wrapped plugin")
set(CLAP_SDK_ROOT "" CACHE STRING "Path to CLAP SDK")
set(VST3_SDK_ROOT "" CACHE STRING "Path to VST3 SDK")
set(AUDIOUNIT_SDK_ROOT "" CACHE STRING "Path to the Apple AUV2 Audio Unit SDK")

function(DetectCLAP)
  if(CLAP_SDK_ROOT STREQUAL "")
	message(STATUS "clap-wrapper: searching CLAP SDK in \"${CMAKE_CURRENT_SOURCE_DIR}\"...")

	if ( CLAP_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap")
	  set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap")
	  message(STATUS "clap-wrapper: CLAP SDK detected in libs subdirectory")
	endif()

		if ( CLAP_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap")
	  set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/clap")
	  message(STATUS "clap-wrapper: CLAP SDK detected in subdirectory")
	endif()
	
	if ( CLAP_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../clap")
	  set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../clap")
	  message(STATUS "clap-wrapper: CLAP SDK detected in parent subdirectory")
	endif()

	if(CLAP_SDK_ROOT STREQUAL "")
		message(FATAL_ERROR "Unable to detect CLAP SDK! Have you set -DCLAP_SDK_ROOT=/path/to/sdk?")
	endif()

	cmake_path(CONVERT "${CLAP_SDK_ROOT}" TO_CMAKE_PATH_LIST CLAP_SDK_ROOT)

	message(STATUS "clap-wrapper: CLAP SDK location: ${CLAP_SDK_ROOT}")
	set(CLAP_SDK_ROOT "${CLAP_SDK_ROOT}" PARENT_SCOPE)

  endif()
endfunction(DetectCLAP)

# allow the programmer to place the VST3 SDK at various places
function(DetectVST3SDK)
  
  if(VST3_SDK_ROOT STREQUAL "")
	message(STATUS "clap-wrapper: searching VST3 SDK in \"${CMAKE_CURRENT_SOURCE_DIR}\"...")

	if ( VST3_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/vst3sdk")
	  set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/vst3sdk")
	  message(STATUS "clap-wrapper: VST3 SDK detected in libs subdirectory")
	endif()

	if ( VST3_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
	  set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
	  message(STATUS "clap-wrapper: VST3 SDK detected in subdirectory")
	endif()

	if ( VST3_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk")
	  set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk")
	  message(STATUS "clap-wrapper: VST3 SDK detected in parent directory")
	endif()

	if(VST3_SDK_ROOT STREQUAL "")
		message(FATAL_ERROR "Unable to detect VST3 SDK. Have you set -DVST3_SDK_ROOT=/path/to/sdk?")
	endif()
  
  endif()

  cmake_path(CONVERT "${VST3_SDK_ROOT}" TO_CMAKE_PATH_LIST VST3_SDK_ROOT)
  message(STATUS "clap-wrapper: VST3 SDK location: ${VST3_SDK_ROOT}")
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
			PARENT_SCOPE
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
	PARENT_SCOPE)


endfunction(DefineCLAPASVST3Sources)

##################### 

# if clap-core is already existent as target, there is no need to do this here
if (NOT TARGET clap-core)
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

DefineCLAPASVST3Sources() 

#####################

function(DetectAudioUnitSDK)
	if(AUDIOUNIT_SDK_ROOT STREQUAL "")
		message(STATUS "clap-wrapper: searching AudioUnit SDK in \"${CMAKE_CURRENT_SOURCE_DIR}\"...")

		if ( AUDIOUNIT_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/AudioUnitSDK")
			set(AUDIOUNIT_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/AudioUnitSDK")
			message(STATUS "clap-wrapper: AudioUnit SDK detected in libs subdirectory")
		endif()

		if ( AUDIOUNIT_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AudioUnitSDK")
			set(AUDIOUNIT_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/AudioUnitSDK")
			message(STATUS "clap-wrapper: AudioUnit SDK detected in subdirectory")
		endif()

		if ( AUDIOUNIT_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../AudioUnitSDK")
			set(AUDIOUNIT_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AudioUnitSDK")
			message(STATUS "clap-wrapper: AudioUnit SDK detected in parent subdirectory")
		endif()

		if(AUDIOUNIT_SDK_ROOT STREQUAL "")
			message(FATAL_ERROR "Unable to detect AudioUnitSDK! Have you set -DCLAP_SDK_ROOT=/path/to/sdk?")
		endif()

		cmake_path(CONVERT "${AUDIOUNIT_SDK_ROOT}" TO_CMAKE_PATH_LIST AUDIOUNIT_SDK_ROOT)

		message(STATUS "clap-wrapper: AudioUnit SDK location: ${AUDIOUNIT_SDK_ROOT}")
		set(CLAP_SDK_ROOT "${AUDIOUNIT_SDK_ROOT}" PARENT_SCOPE)
	else()
		cmake_path(CONVERT "${AUDIOUNIT_SDK_ROOT}" TO_CMAKE_PATH_LIST AUDIOUNIT_SDK_ROOT)
		message(STATUS "clap-wrapper: AudioUnit SDK location: ${AUDIOUNIT_SDK_ROOT}")
	endif()
endfunction(DetectAudioUnitSDK)

if (APPLE)
	if (${CLAP_WRAPPER_BUILD_AUV2})
		DetectAudioUnitSDK()

		set(AUSDK_SRC ${AUDIOUNIT_SDK_ROOT}/src/AudioUnitSDK)
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
endif()

# define platforms

if (APPLE)
	set(PLATFORM "MAC")
elseif(UNIX)
	set(PLATFORM "LIN")
elseif(WIN32)
	set(PLATFORM "WIN")
endif()
