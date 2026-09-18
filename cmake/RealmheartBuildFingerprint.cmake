# Per-component build fingerprints: what each component was built against.
#
# The helper is deliberately declarative: each component passes the canonical
# manifest dependency ids together with the versions CMake actually resolved.
# The resulting JSON installs with the release resources and is read back by
# the Doctor for build-time-versus-current comparisons.

function(_realmheart_json_escape input output)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set("${output}" "${escaped}" PARENT_SCOPE)
endfunction()

function(realmheart_add_build_fingerprint component)
    cmake_parse_arguments(ARG "" "" "DEPENDENCIES" ${ARGN})
    if(NOT ARG_DEPENDENCIES)
        return()
    endif()

    set(entries "")
    foreach(pair IN LISTS ARG_DEPENDENCIES)
        string(FIND "${pair}" ":" separator)
        if(separator LESS 0)
            message(WARNING "Realmheart build fingerprint ${component}: expected dependency_id:version, got '${pair}'")
            continue()
        endif()
        string(SUBSTRING "${pair}" 0 ${separator} dependency_id)
        math(EXPR version_offset "${separator} + 1")
        string(SUBSTRING "${pair}" ${version_offset} -1 dependency_version)
        if(dependency_id STREQUAL "" OR dependency_version STREQUAL "")
            continue()
        endif()
        _realmheart_json_escape("${dependency_version}" dependency_version)
        if(entries)
            string(APPEND entries ",\n")
        endif()
        string(APPEND entries "    \"${dependency_id}\": \"${dependency_version}\"")
    endforeach()

    if(NOT entries)
        return()
    endif()

    _realmheart_json_escape("${PROJECT_VERSION}" realmheart_version)
    _realmheart_json_escape("${CMAKE_CXX_COMPILER_ID}" compiler_id)
    _realmheart_json_escape("${CMAKE_CXX_COMPILER_VERSION}" compiler_version)
    _realmheart_json_escape("${CMAKE_GENERATOR}" generator)
    _realmheart_json_escape("${CMAKE_BUILD_TYPE}" build_type)

    set(output "${CMAKE_BINARY_DIR}/build-fingerprints/${component}.json")
    file(WRITE "${output}"
"{
  \"format_version\": 1,
  \"component_id\": \"${component}\",
  \"realmheart_version\": \"${realmheart_version}\",
  \"generator\": \"${generator}\",
  \"build_type\": \"${build_type}\",
  \"compiler\": {
    \"id\": \"${compiler_id}\",
    \"version\": \"${compiler_version}\"
  },
  \"dependencies\": {
${entries}
  }
}
")
    set_property(GLOBAL APPEND PROPERTY REALMHEART_BUILD_FINGERPRINT_FILES "${output}")
endfunction()
