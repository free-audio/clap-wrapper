# wrapper components
# for VST3
#
# 2022 defiant nerd
#
# options
set(CLAP_SDK_ROOT "" CACHE STRING "Path to CLAP SDK")
set(VST3_SDK_ROOT "" CACHE STRING "Path to VST3 SDK")

function(DetectCLAP)
  if(CLAP_SDK_ROOT STREQUAL "")
	message(STATUS "searching CLAP SDK in ´${CMAKE_CURRENT_SOURCE_DIR}´...")

	if ( CLAP_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap")
	  set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap")
	  message(STATUS "CLAP SDK detected in libs subdirectory")
	endif()

		if ( CLAP_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap")
	  set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/clap")
	  message(STATUS "CLAP SDK detected in libs subdirectory")
	endif()
	
	if ( CLAP_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../clap")
	  set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../clap")
	  message(STATUS "CLAP SDK detected in parent subdirectory")
	endif()

	if(CLAP_SDK_ROOT STREQUAL "")
		message(FATAL_ERROR "Unable to detect CLAP SDK!")
	endif()

	cmake_path(CONVERT "${CLAP_SDK_ROOT}" TO_CMAKE_PATH_LIST CLAP_SDK_ROOT)

	message(STATUS "CLAP SDK at ${CLAP_SDK_ROOT}")
	set(CLAP_SDK_ROOT "${CLAP_SDK_ROOT}" PARENT_SCOPE)

  endif()
endfunction(DetectCLAP)

# allow the programmer to place the VST3 SDK at various places
function(DetectVST3SDK)
  
  if(VST3_SDK_ROOT STREQUAL "")
	message(STATUS "searching VST3 SDK in ´${CMAKE_CURRENT_SOURCE_DIR}´...")

	if ( VST3_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/vst3sdk")
	  set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/vst3sdk")
	  message(STATUS "VST3 SDK detected in libs subdirectory")
	endif()

	if ( VST3_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
	  set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
	  message(STATUS "VST3 SDK detected in subdirectory")
	endif()

	if ( VST3_SDK_ROOT STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk")
	  set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk")
	  message(STATUS "VST3 SDK detected in parent directory")
	endif()

	if(VST3_SDK_ROOT STREQUAL "")
		message(FATAL_ERROR "Unable to detect VST3 SDK. Have you set -DVST3_SDK_ROOT=/path/to/sdk?")
	endif()
  
  endif()

  cmake_path(CONVERT "${VST3_SDK_ROOT}" TO_CMAKE_PATH_LIST VST3_SDK_ROOT)
  message(STATUS "VST3 SDK location: ${VST3_SDK_ROOT}")
  set(VST3_SDK_ROOT "${VST3_SDK_ROOT}" PARENT_SCOPE)

endfunction()

function(DefineVST3Sources)
set(vst3sources
	${VST3_SDK_ROOT}/public.sdk/source/main/pluginfactory.cpp

	${VST3_SDK_ROOT}/public.sdk/source/common/commoniids.cpp
	${VST3_SDK_ROOT}/public.sdk/source/common/memorystream.cpp
	${VST3_SDK_ROOT}/public.sdk/source/common/pluginview.cpp

	${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstaudioeffect.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstcomponentbase.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstcomponent.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.h
	#${VST3_SDK_ROOT}/public.sdk/source/vst/vsteditcontroller.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstbus.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstparameters.cpp

	${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp
	${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp
	${VST3_SDK_ROOT}/pluginterfaces/base/ustring.cpp

	${VST3_SDK_ROOT}/base/source/baseiids.cpp
	${VST3_SDK_ROOT}/base/source/fobject.cpp
	${VST3_SDK_ROOT}/base/source/fstring.cpp
	${VST3_SDK_ROOT}/base/source/fbuffer.cpp
	${VST3_SDK_ROOT}/base/source/fdynlib.cpp
	${VST3_SDK_ROOT}/base/source/fdebug.cpp
	${VST3_SDK_ROOT}/base/source/updatehandler.cpp
	${VST3_SDK_ROOT}/base/thread/source/flock.cpp
	${VST3_SDK_ROOT}/base/thread/source/fcondition.cpp
	
	PARENT_SCOPE
)


if(WIN32)
	set(os_wrappersources src/detail/os/windows.cpp)
endif()

if (APPLE)
	set(os_wrappersources src/detail/clap/mac_helpers.mm)
endif()

set(wrappersources
	src/clap_proxy.h
	src/clap_proxy.cpp
	src/wrapasvst3.h
	src/wrapasvst3.cpp
	src/wrapasvst3_version.h
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
	${os_wrappersources}
PARENT_SCOPE)


endfunction(DefineVST3Sources)

##################### 

DetectCLAP()

add_subdirectory(${CLAP_SDK_ROOT} ${CMAKE_BINARY_DIR}/clap EXCLUDE_FROM_ALL)

DetectVST3SDK()

if(NOT EXISTS "${VST3_SDK_ROOT}/public.sdk")
    message(FATAL_ERROR "There is no VST3 SDK at ${VST3_SDK_ROOT} ")
endif()

add_subdirectory(${VST3_SDK_ROOT}  ${CMAKE_BINARY_DIR}/vst3sdk EXCLUDE_FROM_ALL)

DefineVST3Sources()

#####################

# define platforms

if (APPLE)
	set(PLATFORM "MAC")
elseif(UNIX)
	set(PLATFORM "LIN")
elseif(WIN32)
	set(PLATFORM "WIN")
endif()
