cmake_minimum_required(VERSION 3.25)
project(Realmheart VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REALMHEART_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(REALMHEART_ENABLE_NATIVE_WALLPAPER "Build the Wayland/EGL wallpaper renderer when dependencies are available" ON)
set(REALMHEART_WLR_LAYER_SHELL_XML "" CACHE FILEPATH "Path to wlr-layer-shell-unstable-v1.xml")
set(REALMHEART_XDG_SHELL_XML "" CACHE FILEPATH "Path to stable xdg-shell.xml")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
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
    src/services/WallpaperService.cpp
    src/services/SessionManager.cpp
    src/services/BatteryService.cpp
    src/services/MediaService.cpp
)

target_include_directories(realmheart PRIVATE src)

add_library(realmheart_ui STATIC
    src/ui/AssetResolver.cpp
    src/ui/ImageFileFilters.cpp
    src/ui/GtkApp.cpp
    src/ui/LayerSurface.cpp
    src/ui/wallpaper/WallpaperBackend.cpp
    src/ui/wallpaper/WallpaperSurface.cpp
    src/ui/wallpaper/GtkWallpaperBackend.cpp
    src/ui/wallpaper/NativeWallpaperBackend.cpp
    src/ui/wallpaper/WallpaperController.cpp
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
    PkgConfig::GIO
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

# The native renderer is deliberately optional. Realmheart always retains the GTK
# backend, so a missing Wayland/EGL development package never breaks the shell.
set(REALMHEART_NATIVE_WALLPAPER_BUILT OFF)
if(REALMHEART_ENABLE_NATIVE_WALLPAPER)
    pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_EGL QUIET IMPORTED_TARGET wayland-egl)
    pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)
    pkg_check_modules(GLESV2 QUIET IMPORTED_TARGET glesv2)
    pkg_check_modules(GDK_PIXBUF QUIET IMPORTED_TARGET gdk-pixbuf-2.0)
    pkg_check_modules(WLR_PROTOCOLS QUIET wlr-protocols)
    pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
    find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner)

    if(NOT REALMHEART_WLR_LAYER_SHELL_XML AND WLR_PROTOCOLS_FOUND)
        pkg_get_variable(WLR_PROTOCOLS_DATADIR wlr-protocols pkgdatadir)
        set(_realmheart_wlr_xml
            "${WLR_PROTOCOLS_DATADIR}/unstable/wlr-layer-shell-unstable-v1.xml"
        )
        if(EXISTS "${_realmheart_wlr_xml}")
            set(REALMHEART_WLR_LAYER_SHELL_XML "${_realmheart_wlr_xml}")
        endif()
    endif()

    if(NOT REALMHEART_WLR_LAYER_SHELL_XML)
        find_file(_REALMHEART_WLR_LAYER_SHELL_XML
            NAMES wlr-layer-shell-unstable-v1.xml
            PATHS /usr/share/wlr-protocols /usr/local/share/wlr-protocols
            PATH_SUFFIXES unstable
        )
        if(_REALMHEART_WLR_LAYER_SHELL_XML)
            set(
                REALMHEART_WLR_LAYER_SHELL_XML
                "${_REALMHEART_WLR_LAYER_SHELL_XML}"
                CACHE FILEPATH "Path to wlr-layer-shell-unstable-v1.xml" FORCE
            )
        endif()
    endif()

    if(NOT REALMHEART_XDG_SHELL_XML AND WAYLAND_PROTOCOLS_FOUND)
        pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
        set(_realmheart_xdg_xml
            "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml"
        )
        if(EXISTS "${_realmheart_xdg_xml}")
            set(REALMHEART_XDG_SHELL_XML "${_realmheart_xdg_xml}")
        endif()
    endif()

    if(NOT REALMHEART_XDG_SHELL_XML)
        find_file(_REALMHEART_XDG_SHELL_XML
            NAMES xdg-shell.xml
            PATHS
                /usr/share/wayland-protocols
                /usr/local/share/wayland-protocols
            PATH_SUFFIXES stable/xdg-shell
        )
        if(_REALMHEART_XDG_SHELL_XML)
            set(
                REALMHEART_XDG_SHELL_XML
                "${_REALMHEART_XDG_SHELL_XML}"
                CACHE FILEPATH "Path to stable xdg-shell.xml" FORCE
            )
        endif()
    endif()

    if(WAYLAND_CLIENT_FOUND AND WAYLAND_EGL_FOUND AND EGL_FOUND AND
       GLESV2_FOUND AND GDK_PIXBUF_FOUND AND WAYLAND_SCANNER_EXECUTABLE AND
       REALMHEART_WLR_LAYER_SHELL_XML AND REALMHEART_XDG_SHELL_XML)
        set(REALMHEART_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-protocols")
        file(MAKE_DIRECTORY "${REALMHEART_PROTOCOL_DIR}")

        # wayland-scanner generates a C API. The layer-shell protocol names one
        # request argument "namespace", which is valid C but a reserved C++
        # keyword. Generate from a local, wire-compatible copy whose argument
        # identifier is renamed to "namespace_".
        set(REALMHEART_LAYER_SHELL_CPP_XML
            "${REALMHEART_PROTOCOL_DIR}/wlr-layer-shell-unstable-v1-cpp.xml"
        )
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${REALMHEART_WLR_LAYER_SHELL_XML}"
        )
        file(READ
            "${REALMHEART_WLR_LAYER_SHELL_XML}"
            _realmheart_layer_shell_xml
        )
        string(REPLACE
            "<arg name=\"namespace\""
            "<arg name=\"namespace_\""
            _realmheart_layer_shell_xml
            "${_realmheart_layer_shell_xml}"
        )
        file(WRITE
            "${REALMHEART_LAYER_SHELL_CPP_XML}"
            "${_realmheart_layer_shell_xml}"
        )

        set(REALMHEART_LAYER_SHELL_HEADER
            "${REALMHEART_PROTOCOL_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h"
        )
        set(REALMHEART_LAYER_SHELL_CODE
            "${REALMHEART_PROTOCOL_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
        )

        # wlr-layer-shell references the xdg_popup interface. The interface
        # definition lives in the stable xdg-shell protocol, so its generated
        # protocol code must be linked into the renderer as well.
        set(REALMHEART_XDG_SHELL_CODE
            "${REALMHEART_PROTOCOL_DIR}/xdg-shell-protocol.c"
        )

        add_custom_command(
            OUTPUT "${REALMHEART_LAYER_SHELL_HEADER}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header
                    "${REALMHEART_LAYER_SHELL_CPP_XML}"
                    "${REALMHEART_LAYER_SHELL_HEADER}"
            DEPENDS "${REALMHEART_LAYER_SHELL_CPP_XML}"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${REALMHEART_LAYER_SHELL_CODE}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
                    "${REALMHEART_LAYER_SHELL_CPP_XML}"
                    "${REALMHEART_LAYER_SHELL_CODE}"
            DEPENDS "${REALMHEART_LAYER_SHELL_CPP_XML}"
            VERBATIM
        )

        add_custom_command(
            OUTPUT "${REALMHEART_XDG_SHELL_CODE}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
                    "${REALMHEART_XDG_SHELL_XML}"
                    "${REALMHEART_XDG_SHELL_CODE}"
            DEPENDS "${REALMHEART_XDG_SHELL_XML}"
            VERBATIM
        )

        add_executable(realmheart-wallpaper-renderer
            src/wallpaper-native/main.cpp
            src/wallpaper-native/NativeWallpaperRenderer.cpp
            "${REALMHEART_LAYER_SHELL_HEADER}"
            "${REALMHEART_LAYER_SHELL_CODE}"
            "${REALMHEART_XDG_SHELL_CODE}"
        )
        target_include_directories(realmheart-wallpaper-renderer PRIVATE
            src
            "${REALMHEART_PROTOCOL_DIR}"
        )
        target_link_libraries(realmheart-wallpaper-renderer PRIVATE
            PkgConfig::WAYLAND_CLIENT
            PkgConfig::WAYLAND_EGL
            PkgConfig::EGL
            PkgConfig::GLESV2
            PkgConfig::GDK_PIXBUF
        )
        target_compile_options(realmheart-wallpaper-renderer PRIVATE
            -Wall -Wextra -Wpedantic
            $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
        )
        set(REALMHEART_NATIVE_WALLPAPER_BUILT ON)
    else()
        message(STATUS
            "Native wallpaper renderer disabled: install wayland-client, wayland-egl, EGL, GLESv2, gdk-pixbuf, wayland-scanner, wlr-protocols and wayland-protocols"
        )
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
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

    add_executable(realmheart_battery_service_tests
        tests/BatteryServiceTests.cpp
        src/services/BatteryService.cpp
    )
    target_include_directories(realmheart_battery_service_tests PRIVATE src)
    target_compile_options(realmheart_battery_service_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_notes_tests
        tests/NotesServiceTests.cpp
        src/services/NotesService.cpp
    )
    target_include_directories(realmheart_notes_tests PRIVATE src)
    target_compile_options(realmheart_notes_tests PRIVATE
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
        src/ui/ImageFileFilters.cpp
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

    add_executable(realmheart_wallpaper_service_tests
        tests/WallpaperServiceTests.cpp
        src/services/WallpaperService.cpp
    )
    target_link_libraries(realmheart_wallpaper_service_tests PRIVATE GTest::gtest_main)
    target_include_directories(realmheart_wallpaper_service_tests PRIVATE src)
    target_compile_options(realmheart_wallpaper_service_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_wallpaper_backend_tests
        tests/WallpaperBackendTests.cpp
        src/ui/wallpaper/WallpaperBackend.cpp
    )
    target_include_directories(realmheart_wallpaper_backend_tests PRIVATE src)
    target_compile_options(realmheart_wallpaper_backend_tests PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
    )

    add_executable(realmheart_utility_manager_tests
        tests/UtilityManagerTests.cpp
        src/core/Command.cpp
        src/services/UtilityManager.cpp
        src/services/WallpaperService.cpp
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
    add_test(NAME realmheart_battery_service COMMAND realmheart_battery_service_tests)
    add_test(NAME realmheart_notes_persistence COMMAND realmheart_notes_tests)
    add_test(NAME realmheart_notification_history COMMAND realmheart_notification_tests)
    add_test(NAME realmheart_notification_server COMMAND realmheart_notification_server_tests)
    add_test(NAME realmheart_vertical_bar_model COMMAND realmheart_vertical_bar_model_tests)
    add_test(NAME realmheart_ui_foundation COMMAND realmheart_ui_foundation_tests)
    add_test(NAME realmheart_wallpaper_service COMMAND realmheart_wallpaper_service_tests)
    add_test(NAME realmheart_wallpaper_backend COMMAND realmheart_wallpaper_backend_tests)
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
if(TARGET realmheart-wallpaper-renderer)
    install(TARGETS realmheart-wallpaper-renderer RUNTIME DESTINATION bin)
endif()
