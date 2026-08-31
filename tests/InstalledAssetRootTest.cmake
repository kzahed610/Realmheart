if(NOT DEFINED REALMHEART_BUILD_DIR OR
   NOT DEFINED REALMHEART_STAGE_DIR OR
   NOT DEFINED REALMHEART_TEST_EXECUTABLE OR
   NOT DEFINED REALMHEART_INSTALL_FULL_DATADIR)
    message(FATAL_ERROR "InstalledAssetRootTest.cmake is missing required -D arguments")
endif()

file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
file(MAKE_DIRECTORY "${REALMHEART_STAGE_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "DESTDIR=${REALMHEART_STAGE_DIR}"
            "${CMAKE_COMMAND}" --install "${REALMHEART_BUILD_DIR}"
    RESULT_VARIABLE install_status
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_status EQUAL 0)
    message(FATAL_ERROR
        "Staged Realmheart install failed (${install_status})\n"
        "stdout:\n${install_stdout}\n"
        "stderr:\n${install_stderr}"
    )
endif()

set(asset_root
    "${REALMHEART_STAGE_DIR}${REALMHEART_INSTALL_FULL_DATADIR}/realmheart/assets"
)
if(NOT IS_DIRECTORY "${asset_root}")
    message(FATAL_ERROR "Staged Realmheart asset root is missing: ${asset_root}")
endif()

execute_process(
    COMMAND "${REALMHEART_TEST_EXECUTABLE}" "${asset_root}"
    RESULT_VARIABLE probe_status
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
)
if(NOT probe_status EQUAL 0)
    message(FATAL_ERROR
        "Installed asset resolver/package probe failed (${probe_status})\n"
        "stdout:\n${probe_stdout}\n"
        "stderr:\n${probe_stderr}"
    )
endif()

message(STATUS "${probe_stdout}")
file(REMOVE_RECURSE "${REALMHEART_STAGE_DIR}")
