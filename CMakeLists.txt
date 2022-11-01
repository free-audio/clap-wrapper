cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)

#
#		a lot of work in progress and things left todo
#

project(clap-as-vst3 
	LANGUAGES C CXX
	VERSION 0.0.3
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

# create plugin link
set(SMTG_CREATE_PLUGIN_LINK ON)

# discover the plugin paths and enable them
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

add_library(clap-wrapper-extensions INTERFACE)
target_include_directories(clap-wrapper-extensions INTERFACE include)

# ------------------------------------------------------------------------------

# provide the CLAP_WRAPPER_OUTPUT_NAME to specify the matching plugin name
if((NOT CLAP_WRAPPER_OUTPUT_NAME ) OR (CLAP_WRAPPER_OUTPUT_NAME STREQUAL ""))
  set(pluginname "clapasvst3")
  message(WARNING "CLAP_WRAPPER_OUTPUT_NAME not set - continuing with a default name `clapasvst3`")
else()
  string(MAKE_C_IDENTIFIER ${CLAP_WRAPPER_OUTPUT_NAME} pluginname)  
  message(STATUS "plugin name is ${CLAP_WRAPPER_OUTPUT_NAME}")
endif()

set(pluginname ${pluginname}_as_vst3)

add_definitions(-D${PLATFORM}=1)

message(STATUS "platform is ${PLATFORM}")
#
smtg_enable_vst3_sdk()

set(SRCLIST 
	${wrappersources_vst3}
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
	${VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.h
)

smtg_add_vst3plugin(${pluginname} 
	PACKAGE_NAME ${CLAP_WRAPPER_OUTPUT_NAME}
	SOURCES_LIST ${SRCLIST}
)

smtg_target_add_library_main(${pluginname})

set_target_properties(${pluginname} PROPERTIES LIBRARY_OUTPUT_NAME "${CLAP_WRAPPER_OUTPUT_NAME}")

# link both SDKs, CLAP and VST3
target_link_libraries(${pluginname} PRIVATE clap-core sdk clap-wrapper-extensions)
message(STATUS " current source dir: ${CMAKE_CURRENT_SOURCE_DIR} ")
target_include_directories(${pluginname} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if (APPLE)
	target_link_libraries (${pluginname} PUBLIC "-framework Foundation" "-framework CoreFoundation")
endif()

if (UNIX AND NOT APPLE)
	target_link_libraries(${pluginname} PUBLIC "-ldl")
	target_link_libraries(${pluginname} PRIVATE "-Wl,--no-undefined")
endif()


