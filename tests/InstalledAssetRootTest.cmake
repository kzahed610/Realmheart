if(NOT DEFINED REALMHEART_BUILD_DIR OR
   NOT DEFINED REALMHEART_STAGE_DIR OR
   NOT DEFINED REALMHEART_TEST_EXECUTABLE OR
   NOT DEFINED REALMHEART_INSTALL_PREFIX OR
   NOT DEFINED REALMHEART_INSTALL_DATADIR)
    message(FATAL_ERROR "InstalledAssetRootTest.cmake is missing required -D arguments")
endif()

file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
file(MAKE_DIRECTORY "${REALMHEART_STAGE_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "DESTDIR=${REALMHEART_STAGE_DIR}"
            "REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL=1"
            "${CMAKE_COMMAND}" --install "${REALMHEART_BUILD_DIR}"
            --prefix "${REALMHEART_INSTALL_PREFIX}"
    RESULT_VARIABLE install_status
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_status EQUAL 0)
    file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
    message(FATAL_ERROR
        "Staged Realmheart install failed (${install_status})\n"
        "stdout:\n${install_stdout}\n"
        "stderr:\n${install_stderr}"
    )
endif()

set(asset_root
    "${REALMHEART_STAGE_DIR}${REALMHEART_INSTALL_PREFIX}/${REALMHEART_INSTALL_DATADIR}/realmheart/assets"
)
if(NOT IS_DIRECTORY "${asset_root}")
    file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
    message(FATAL_ERROR "Staged Realmheart asset root is missing: ${asset_root}")
endif()

set(auth_helper
    "${REALMHEART_STAGE_DIR}${REALMHEART_INSTALL_PREFIX}/libexec/realmheart/realmheart-auth-helper"
)
if(NOT EXISTS "${auth_helper}" OR IS_DIRECTORY "${auth_helper}" OR IS_SYMLINK "${auth_helper}")
    file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
    message(FATAL_ERROR "Staged Realmheart auth helper is missing or not regular: ${auth_helper}")
endif()
execute_process(
    COMMAND stat -c %a "${auth_helper}"
    RESULT_VARIABLE helper_stat_status
    OUTPUT_VARIABLE helper_mode
    ERROR_VARIABLE helper_stat_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT helper_stat_status EQUAL 0 OR NOT helper_mode STREQUAL "4755")
    file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
    message(FATAL_ERROR
        "Staged Realmheart auth helper has wrong mode (${helper_mode}): ${auth_helper}\n"
        "stat error: ${helper_stat_error}"
    )
endif()

execute_process(
    COMMAND "${REALMHEART_TEST_EXECUTABLE}" "${asset_root}"
    RESULT_VARIABLE probe_status
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
)
if(NOT probe_status EQUAL 0)
    file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
    message(FATAL_ERROR
        "Installed asset resolver/package probe failed (${probe_status})\n"
        "stdout:\n${probe_stdout}\n"
        "stderr:\n${probe_stderr}"
    )
endif()

message(STATUS "${probe_stdout}")
file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
