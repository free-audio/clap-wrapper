cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)

#
#		a lot of work in progress and things left todo
#

project(clap-as-vst3 
	LANGUAGES C CXX
	VERSION 0.0.1
	DESCRIPTION "CLAP as VST3"
)

# plugin UI will be provided by the CLAP
set(SMTG_ADD_VSTGUI OFF)

# we want a BUNDLE, not the folder format
set(SMTG_CREATE_MODULE_INFO OFF)

# we want a DLL, not the folder format
set(SMTG_CREATE_BUNDLE_FOR_WINDOWS OFF)

# not now - but can be useful to activate
set(SMTG_RUN_VST_VALIDATOR OFF)

# THIS we want.
set(SMTG_CREATE_PLUGIN_LINK ON)

include(cmake/enable_sdks.cmake)

# ------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)

# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13 CACHE STRING "Build for 10.13")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------------------


if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "CMAKE_BUILD_TYPE Type Unspecified; picking Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

message(STATUS "executing at ${CMAKE_CURRENT_SOURCE_DIR}")

# ------------------------------------------------------------------------------

# this will be a CMake argument
set(project "clapasvst3")

add_definitions(-D${PLATFORM}=1)

message(STATUS "platform is ${PLATFORM}")
#
smtg_enable_vst3_sdk()

smtg_add_vst3plugin(${project}
	${wrappersources_vst3}
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.h
)

smtg_target_add_library_main(${project})

# link both SDKs
target_link_libraries(${project} PRIVATE clap-core sdk)
message(STATUS " current source dir: ${CMAKE_CURRENT_SOURCE_DIR} ")
target_include_directories(${project} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${project} PUBLIC ${VST3_SDK_ROOT})

if (APPLE)
	target_link_libraries (${project} PUBLIC "-framework Foundation" "-framework CoreFoundation")
endif()

if (UNIX AND NOT APPLE)
	target_link_libraries (${project} PUBLIC "-ldl")
endif()

# a debuggable executable to test - will be removed later and disabled by default
if(FALSE)
add_executable(${project}_as_executable
	src/wramain.cpp
	${wrappersources_vst3}
	${vst3sources}
)
target_link_libraries(${project}_as_executable PRIVATE clap-core sdk)
target_include_directories(${project}_as_executable PUBLIC ${VST3_SDK_ROOT})

if (APPLE)
	target_link_libraries (${project}_as_executable PUBLIC "-framework Foundation" "-framework CoreFoundation")
endif()

if (UNIX AND NOT APPLE)
	target_link_libraries (${project}_as_executable PUBLIC "-ldl")
endif()

endif()

# smtg_target_configure_version_file(${project})


