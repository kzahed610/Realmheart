cmake_minimum_required(VERSION 3.25)
project(Realmheart VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REALMHEART_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GTK4_LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)

add_executable(realmheart
    src/main.cpp
    src/core/Command.cpp
    src/core/Diagnostics.cpp
    src/core/ModuleRegistry.cpp
    src/core/ShellCommand.cpp
    src/core/ShellControl.cpp
    src/services/Audio.cpp
    src/services/Bluetooth.cpp
    src/services/Brightness.cpp
    src/services/GameMode.cpp
    src/services/HyprlandWorkspaces.cpp
    src/services/KeepAwake.cpp
    src/services/NightLight.cpp
    src/services/NotificationDaemon.cpp
    src/services/NotificationServer.cpp
    src/services/Notifications.cpp
    src/services/PowerProfiles.cpp
    src/services/ScopeModules.cpp
    src/services/RightSidebarServices.cpp
    src/services/Wifi.cpp
    src/services/UtilityManager.cpp
    src/services/SessionManager.cpp
    src/services/BatteryService.cpp
    src/services/MediaService.cpp
)

target_include_directories(realmheart PRIVATE src)

add_library(realmheart_ui STATIC
    src/ui/AssetResolver.cpp
    src/ui/GtkApp.cpp
    src/ui/LayerSurface.cpp
    src/ui/ShellApp.cpp
    src/ui/bar/VerticalBar.cpp
    src/ui/bar/VerticalBarModel.cpp
    src/ui/sidebar/RightSidebar.cpp
    src/ui/NotificationToast.cpp
    src/ui/OSDOverlay.cpp
    src/ui/lockscreen/LockScreen.cpp
    src/ui/launcher/LauncherOverlay.cpp
    src/services/LauncherService.cpp
 )

target_sources(realmheart_ui PRIVATE
    src/services/NotesService.cpp
    src/ui/NotesOverlay.cpp
)

target_include_directories(realmheart_ui PRIVATE src)
target_compile_definitions(realmheart_ui PRIVATE
    REALMHEART_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
)

target_link_libraries(realmheart_ui PRIVATE
    PkgConfig::GTK4
    PkgConfig::GTK4_LAYER_SHELL
)

target_compile_options(realmheart PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
)

target_compile_options(realmheart_ui PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
)

target_link_libraries(realmheart PRIVATE
    realmheart_ui
    PkgConfig::GIO
)

include(CTest)
if(BUILD_TESTING)
    add_executable(realmheart_command_tests
        tests/CommandTests.cpp
        src/core/Command.cpp
    )
    target_include_directories(realmheart_command_tests PRIVATE src)
    target_compile_options(realmheart_command_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_shell_command_tests
        tests/ShellCommandTests.cpp
        src/core/ShellCommand.cpp
    )
    target_include_directories(realmheart_shell_command_tests PRIVATE src)
    target_compile_options(realmheart_shell_command_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_shell_runtime_tests
        tests/ShellRuntimeTests.cpp
        src/core/ShellCommand.cpp
        src/core/ShellControl.cpp
    )
    target_include_directories(realmheart_shell_runtime_tests PRIVATE src)
    target_compile_options(realmheart_shell_runtime_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )
    target_link_libraries(realmheart_shell_runtime_tests PRIVATE PkgConfig::GIO)

    add_executable(realmheart_audio_tests
        tests/AudioTests.cpp
        src/core/Command.cpp
        src/services/Audio.cpp
    )
    target_include_directories(realmheart_audio_tests PRIVATE src)
    target_compile_options(realmheart_audio_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_bluetooth_tests
        tests/BluetoothTests.cpp
        src/core/Command.cpp
        src/services/Bluetooth.cpp
    )
    target_include_directories(realmheart_bluetooth_tests PRIVATE src)
    target_compile_options(realmheart_bluetooth_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_brightness_tests
        tests/BrightnessTests.cpp
        src/core/Command.cpp
        src/services/Brightness.cpp
    )
    target_include_directories(realmheart_brightness_tests PRIVATE src)
    target_compile_options(realmheart_brightness_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_gamemode_tests
        tests/GameModeTests.cpp
        src/core/Command.cpp
        src/services/GameMode.cpp
    )
    target_include_directories(realmheart_gamemode_tests PRIVATE src)
    target_compile_options(realmheart_gamemode_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_keep_awake_tests
        tests/KeepAwakeTests.cpp
        src/core/Command.cpp
        src/services/KeepAwake.cpp
    )
    target_include_directories(realmheart_keep_awake_tests PRIVATE src)
    target_compile_options(realmheart_keep_awake_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_night_light_tests
        tests/NightLightTests.cpp
        src/core/Command.cpp
        src/services/NightLight.cpp
        src/services/SessionManager.cpp
        )
    target_include_directories(realmheart_night_light_tests PRIVATE src)
    target_compile_options(realmheart_night_light_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_power_profiles_tests
        tests/PowerProfilesTests.cpp
        src/core/Command.cpp
        src/services/PowerProfiles.cpp
    )
    target_include_directories(realmheart_power_profiles_tests PRIVATE src)
    target_compile_options(realmheart_power_profiles_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_wifi_tests
        tests/WifiTests.cpp
        src/core/Command.cpp
        src/services/Wifi.cpp
    )
    target_include_directories(realmheart_wifi_tests PRIVATE src)
    target_compile_options(realmheart_wifi_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_shell_state_tests
        tests/ShellStateTests.cpp
    )
    target_include_directories(realmheart_shell_state_tests PRIVATE src)
    target_compile_options(realmheart_shell_state_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_workspace_tests
        tests/HyprlandWorkspacesTests.cpp
        src/core/Command.cpp
        src/services/HyprlandWorkspaces.cpp
    )
    target_include_directories(realmheart_workspace_tests PRIVATE src)
    target_compile_options(realmheart_workspace_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_notification_tests
        tests/NotificationsTests.cpp
        src/services/Notifications.cpp
    )
    target_include_directories(realmheart_notification_tests PRIVATE src)
    target_compile_options(realmheart_notification_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_notification_server_tests
        tests/NotificationServerTests.cpp
        src/services/NotificationServer.cpp
        src/services/Notifications.cpp
    )
    target_include_directories(realmheart_notification_server_tests PRIVATE src)
    target_compile_options(realmheart_notification_server_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_notification_daemon_tests
        tests/NotificationDaemonTests.cpp
        src/services/NotificationDaemon.cpp
        src/services/NotificationServer.cpp
        src/services/Notifications.cpp
    )
    target_include_directories(realmheart_notification_daemon_tests PRIVATE src)
    target_compile_options(realmheart_notification_daemon_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )
    target_link_libraries(realmheart_notification_daemon_tests PRIVATE
        PkgConfig::GIO
        Threads::Threads
    )

    add_executable(realmheart_vertical_bar_model_tests
        tests/VerticalBarModelTests.cpp
        src/ui/bar/VerticalBarModel.cpp
    )
    target_include_directories(realmheart_vertical_bar_model_tests PRIVATE src)
    target_compile_options(realmheart_vertical_bar_model_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_ui_foundation_tests
        tests/UiFoundationTests.cpp
        src/ui/AssetResolver.cpp
        src/ui/LayerSurface.cpp
    )
    target_include_directories(realmheart_ui_foundation_tests PRIVATE src)
    target_compile_definitions(realmheart_ui_foundation_tests PRIVATE
        REALMHEART_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/icons/fluent"
    )
    target_compile_options(realmheart_ui_foundation_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )
    target_link_libraries(realmheart_ui_foundation_tests PRIVATE
        PkgConfig::GTK4
        PkgConfig::GTK4_LAYER_SHELL
    )

    add_executable(realmheart_utility_manager_tests
        tests/UtilityManagerTests.cpp
        src/core/Command.cpp
        src/services/UtilityManager.cpp
    )

    add_executable(realmheart_launcher_service_tests
        tests/LauncherServiceTests.cpp
        tests/LauncherServiceCommandTests.cpp
        src/services/LauncherService.cpp
    )
    target_link_libraries(realmheart_launcher_service_tests PRIVATE GTest::gtest_main PkgConfig::GIO)
    target_include_directories(realmheart_launcher_service_tests PRIVATE src)
    target_compile_options(realmheart_launcher_service_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )
    target_include_directories(realmheart_utility_manager_tests PRIVATE src)
    target_compile_options(realmheart_utility_manager_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_test(NAME realmheart_doctor COMMAND realmheart --doctor)
    add_test(NAME realmheart_modules COMMAND realmheart --list-modules)
    add_test(NAME realmheart_workspaces COMMAND realmheart --workspace-status)
    add_test(NAME realmheart_command_runner COMMAND realmheart_command_tests)
    add_test(NAME realmheart_workspace_parser COMMAND realmheart_workspace_tests)
    add_test(NAME realmheart_notification_history COMMAND realmheart_notification_tests)
    add_test(NAME realmheart_notification_server COMMAND realmheart_notification_server_tests)
    add_test(NAME realmheart_vertical_bar_model COMMAND realmheart_vertical_bar_model_tests)
    add_test(NAME realmheart_ui_foundation COMMAND realmheart_ui_foundation_tests)
    add_test(NAME realmheart_utility_manager COMMAND realmheart_utility_manager_tests)
    add_test(NAME realmheart_launcher_service COMMAND realmheart_launcher_service_tests)
    add_test(NAME realmheart_shell_command_parser COMMAND realmheart_shell_command_tests)
    add_test(NAME realmheart_shell_surface_state COMMAND realmheart_shell_state_tests)
    add_test(NAME realmheart_audio_mutation COMMAND realmheart_audio_tests)
    add_test(NAME realmheart_bluetooth_mutation COMMAND realmheart_bluetooth_tests)
    add_test(NAME realmheart_brightness_mutation COMMAND realmheart_brightness_tests)
    add_test(NAME realmheart_gamemode_mutation COMMAND realmheart_gamemode_tests)
    add_test(NAME realmheart_keep_awake_mutation COMMAND realmheart_keep_awake_tests)
    add_test(NAME realmheart_night_light_mutation COMMAND realmheart_night_light_tests)
    add_test(NAME realmheart_power_profile_mutation COMMAND realmheart_power_profiles_tests)
    add_test(NAME realmheart_wifi_mutation COMMAND realmheart_wifi_tests)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session REQUIRED)
    add_test(
        NAME realmheart_shell_control_without_primary
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:realmheart_shell_runtime_tests>
    )
    add_test(
        NAME realmheart_notification_dbus
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:realmheart_notification_daemon_tests>
    )
endif()
install(TARGETS realmheart RUNTIME DESTINATION bin)
