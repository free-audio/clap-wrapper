# merge_plist.cmake - deep-merges a property list into another one
#
#   cmake -DBASE=<plist> -DADDITIONS=<plist> -DOUT=<plist> -P merge_plist.cmake
#
# Writes BASE with every key of ADDITIONS applied to it: a dictionary in both is
# merged key by key, everything else - strings, numbers, booleans, arrays - is
# replaced by the value from ADDITIONS. So an extension can gain a key inside
# NSExtension/NSExtensionAttributes without having to restate the generated
# AudioComponents, while an array is always taken as given.
#
# Uses plutil to go through JSON, which every Info.plist value but <data> and
# <date> survives; those are reported instead of being silently dropped.

foreach(_arg BASE ADDITIONS OUT)
    if (NOT DEFINED ${_arg})
        message(FATAL_ERROR "merge_plist.cmake: ${_arg} is not defined")
    endif()
endforeach()

function(_plist_to_json file outvar)
    execute_process(COMMAND plutil -convert json -o - "${file}"
            OUTPUT_VARIABLE _json
            ERROR_VARIABLE _err
            RESULT_VARIABLE _res)
    if (NOT _res EQUAL 0)
        message(FATAL_ERROR "merge_plist.cmake: cannot read ${file} as JSON (a <data> or <date> value?): ${_err}${_json}")
    endif()
    set(${outvar} "${_json}" PARENT_SCOPE)
endfunction()

# string(JSON GET) hands back scalars decoded, so they have to be encoded again
# before string(JSON SET) can take them.
function(_json_member_as_json object key outvar)
    string(JSON _type TYPE "${object}" "${key}")
    string(JSON _value GET "${object}" "${key}")
    if (_type STREQUAL "STRING")
        string(REPLACE "\\" "\\\\" _value "${_value}")
        string(REPLACE "\"" "\\\"" _value "${_value}")
        string(REPLACE "\n" "\\n" _value "${_value}")
        string(REPLACE "\r" "\\r" _value "${_value}")
        string(REPLACE "\t" "\\t" _value "${_value}")
        set(_value "\"${_value}\"")
    elseif (_type STREQUAL "BOOLEAN")
        if (_value)
            set(_value "true")
        else()
            set(_value "false")
        endif()
    elseif (_type STREQUAL "NULL")
        set(_value "null")
    endif()
    set(${outvar} "${_value}" PARENT_SCOPE)
endfunction()

function(_merge_json base additions outvar)
    set(_result "${base}")
    string(JSON _count LENGTH "${additions}")
    if (_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_i RANGE ${_last})
            string(JSON _key MEMBER "${additions}" ${_i})
            _json_member_as_json("${additions}" "${_key}" _addition)

            string(JSON _addType TYPE "${additions}" "${_key}")
            string(JSON _baseType ERROR_VARIABLE _missing TYPE "${_result}" "${_key}")
            if (_addType STREQUAL "OBJECT" AND _baseType STREQUAL "OBJECT")
                string(JSON _existing GET "${_result}" "${_key}")
                _merge_json("${_existing}" "${_addition}" _addition)
            endif()

            string(JSON _result SET "${_result}" "${_key}" "${_addition}")
        endforeach()
    endif()
    set(${outvar} "${_result}" PARENT_SCOPE)
endfunction()

_plist_to_json("${BASE}" _base)
_plist_to_json("${ADDITIONS}" _additions)

string(JSON _type TYPE "${_additions}")
if (NOT _type STREQUAL "OBJECT")
    message(FATAL_ERROR "merge_plist.cmake: ${ADDITIONS} must hold a dictionary at the top level")
endif()

_merge_json("${_base}" "${_additions}" _merged)

file(WRITE "${OUT}.json" "${_merged}")
execute_process(COMMAND plutil -convert xml1 -o "${OUT}" "${OUT}.json"
        ERROR_VARIABLE _err
        RESULT_VARIABLE _res)
file(REMOVE "${OUT}.json")
if (NOT _res EQUAL 0)
    message(FATAL_ERROR "merge_plist.cmake: cannot write ${OUT}: ${_err}")
endif()
