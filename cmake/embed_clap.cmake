# embed_clap.cmake — find an installed .clap bundle and copy it into the target
# Called as a post-build script with -DCLAP_NAME=<name>.clap -DDST=<destination path>

if(NOT DEFINED CLAP_NAME OR NOT DEFINED DST)
    message(FATAL_ERROR "embed_clap.cmake requires -DCLAP_NAME and -DDST")
endif()

# Search standard CLAP install locations
set(_search_paths
    "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP"
    "/Library/Audio/Plug-Ins/CLAP"
)

set(_found "")
foreach(_dir IN LISTS _search_paths)
    set(_candidate "${_dir}/${CLAP_NAME}")
    if(IS_DIRECTORY "${_candidate}")
        set(_found "${_candidate}")
        break()
    endif()
endforeach()

if(_found STREQUAL "")
    message(WARNING "embed_clap: '${CLAP_NAME}' not found in standard CLAP paths, appex will search at runtime")
    return()
endif()

message(STATUS "embed_clap: copying ${_found} -> ${DST}")
file(COPY "${_found}" DESTINATION "${DST}/..")
