cmake_minimum_required(VERSION 3.25)
project(Realmheart VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REALMHEART_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(REALMHEART_ENABLE_NATIVE_WALLPAPER "Build the Wayland/EGL wallpaper renderer when dependencies are available" ON)
set(REALMHEART_WLR_LAYER_SHELL_XML "" CACHE FILEPATH "Path to wlr-layer-shell-unstable-v1.xml")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(GTK4_LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
pkg_check_modules(GDK_PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)

set(REALMHEART_WARNING_FLAGS
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${REALMHEART_WARNINGS_AS_ERRORS}>:-Werror>
)

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
    src/services/ThemeService.cpp
    src/services/MatugenParser.cpp
)
target_include_directories(realmheart PRIVATE src third_party)
target_compile_options(realmheart PRIVATE ${REALMHEART_WARNING_FLAGS})

add_library(realmheart_ui STATIC
    src/core/TaskExecutor.cpp
    src/services/HyprlandEventMonitor.cpp
    src/ui/AssetResolver.cpp
    src/ui/ImageFileFilters.cpp
    src/ui/GtkApp.cpp
    src/ui/ThemeStyles.cpp
    src/ui/styles/CssModuleLoader.cpp
    src/ui/LayerSurface.cpp
    src/ui/wallpaper/WallpaperBackend.cpp
    src/ui/wallpaper/WallpaperSurface.cpp
    src/ui/wallpaper/GtkWallpaperBackend.cpp
    src/ui/wallpaper/NativeWallpaperBackend.cpp
    src/ui/wallpaper/WallpaperController.cpp
    src/ui/ShellApp.cpp
    src/ui/bar/VerticalBar.cpp
    src/ui/bar/VerticalBarModel.cpp
    src/ui/bar/MediaArtLoader.cpp
    src/ui/bar/WorkspaceWindowTracker.cpp
    src/ui/bar/widgets/BarBackdrop.cpp
    src/ui/bar/widgets/BarIconButton.cpp
    src/ui/bar/widgets/ThemedSvgIcon.cpp
    src/ui/bar/widgets/ClockWidget.cpp
    src/ui/bar/widgets/WorkspaceRune.cpp
    src/ui/bar/widgets/MediaWidget.cpp
    src/ui/bar/widgets/SystemMonitorWidget.cpp
    src/ui/bar/widgets/BatteryWidget.cpp
    src/services/SystemMonitorService.cpp
    src/ui/sidebar/RightSidebar.cpp
    src/ui/NotificationToast.cpp
    src/ui/OSDOverlay.cpp
    src/ui/lockscreen/LockScreen.cpp
    src/ui/launcher/LauncherOverlay.cpp
    src/services/LauncherService.cpp
    src/services/NotesService.cpp
    src/ui/NotesOverlay.cpp
    src/ui/components/LabelWidget.cpp
    src/ui/components/ToggleWidget.cpp
    src/ui/components/SliderWidget.cpp
    src/ui/components/ButtonWidget.cpp
    src/ui/components/NotificationWidget.cpp
)
target_include_directories(realmheart_ui PRIVATE src third_party)
target_compile_definitions(realmheart_ui PRIVATE
    REALMHEART_INSTALL_ASSET_DIR="${CMAKE_INSTALL_FULL_DATADIR}/realmheart/assets"
    REALMHEART_SOURCE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    REALMHEART_INSTALL_STYLE_DIR="${CMAKE_INSTALL_FULL_DATADIR}/realmheart/styles"
    REALMHEART_SOURCE_STYLE_DIR="${CMAKE_SOURCE_DIR}/styles"
)
target_compile_options(realmheart_ui PRIVATE ${REALMHEART_WARNING_FLAGS})
target_link_libraries(realmheart_ui PRIVATE
    PkgConfig::GIO
    PkgConfig::GTK4
    PkgConfig::GTK4_LAYER_SHELL
    PkgConfig::GDK_PIXBUF
    Threads::Threads
)

target_link_libraries(realmheart PRIVATE
    realmheart_ui
    PkgConfig::GIO
    Threads::Threads
)

set(REALMHEART_NATIVE_WALLPAPER_BUILT OFF)
if(REALMHEART_ENABLE_NATIVE_WALLPAPER)
    pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_EGL QUIET IMPORTED_TARGET wayland-egl)
    pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)
    pkg_check_modules(GLESV2 QUIET IMPORTED_TARGET glesv2)
    pkg_check_modules(WLR_PROTOCOLS QUIET wlr-protocols)
    pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
    find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner)

    if(NOT REALMHEART_WLR_LAYER_SHELL_XML AND WLR_PROTOCOLS_FOUND)
        pkg_get_variable(WLR_PROTOCOLS_DATADIR wlr-protocols pkgdatadir)
        set(_realmheart_wlr_xml "${WLR_PROTOCOLS_DATADIR}/unstable/wlr-layer-shell-unstable-v1.xml")
        if(EXISTS "${_realmheart_wlr_xml}")
            set(REALMHEART_WLR_LAYER_SHELL_XML "${_realmheart_wlr_xml}")
        endif()
    endif()

    if(NOT REALMHEART_XDG_SHELL_XML AND WAYLAND_PROTOCOLS_FOUND)
        pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
        set(_realmheart_xdg_xml "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml")
        if(EXISTS "${_realmheart_xdg_xml}")
            set(REALMHEART_XDG_SHELL_XML "${_realmheart_xdg_xml}")
        endif()
    endif()

    if(WAYLAND_CLIENT_FOUND AND WAYLAND_EGL_FOUND AND EGL_FOUND AND
       GLESV2_FOUND AND GDK_PIXBUF_FOUND AND WAYLAND_SCANNER_EXECUTABLE AND
       REALMHEART_WLR_LAYER_SHELL_XML AND REALMHEART_XDG_SHELL_XML)
        set(REALMHEART_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-protocols")
        file(MAKE_DIRECTORY "${REALMHEART_PROTOCOL_DIR}")

        # The protocol XML uses a C++ keyword as an argument name. Generate from a
        # build-local copy with that identifier renamed; wire compatibility is unchanged.
        set(REALMHEART_LAYER_SHELL_CPP_XML "${REALMHEART_PROTOCOL_DIR}/wlr-layer-shell-unstable-v1-cpp.xml")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REALMHEART_WLR_LAYER_SHELL_XML}")
        file(READ "${REALMHEART_WLR_LAYER_SHELL_XML}" _realmheart_layer_shell_xml)
        string(REPLACE "<arg name=\"namespace\"" "<arg name=\"namespace_\"" _realmheart_layer_shell_xml "${_realmheart_layer_shell_xml}")
        file(WRITE "${REALMHEART_LAYER_SHELL_CPP_XML}" "${_realmheart_layer_shell_xml}")

        set(REALMHEART_LAYER_SHELL_HEADER "${REALMHEART_PROTOCOL_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h")
        set(REALMHEART_LAYER_SHELL_CODE "${REALMHEART_PROTOCOL_DIR}/wlr-layer-shell-unstable-v1-protocol.c")
        add_custom_command(
            OUTPUT "${REALMHEART_LAYER_SHELL_HEADER}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${REALMHEART_LAYER_SHELL_CPP_XML}" "${REALMHEART_LAYER_SHELL_HEADER}"
            DEPENDS "${REALMHEART_LAYER_SHELL_CPP_XML}"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${REALMHEART_LAYER_SHELL_CODE}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${REALMHEART_LAYER_SHELL_CPP_XML}" "${REALMHEART_LAYER_SHELL_CODE}"
            DEPENDS "${REALMHEART_LAYER_SHELL_CPP_XML}"
            VERBATIM
        )

        set(REALMHEART_XDG_SHELL_CODE "${REALMHEART_PROTOCOL_DIR}/xdg-shell-protocol.c")
        add_custom_command(
            OUTPUT "${REALMHEART_XDG_SHELL_CODE}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${REALMHEART_XDG_SHELL_XML}" "${REALMHEART_XDG_SHELL_CODE}"
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
        target_include_directories(realmheart-wallpaper-renderer PRIVATE src "${REALMHEART_PROTOCOL_DIR}")
        target_link_libraries(realmheart-wallpaper-renderer PRIVATE
            PkgConfig::WAYLAND_CLIENT
            PkgConfig::WAYLAND_EGL
            PkgConfig::EGL
            PkgConfig::GLESV2
            PkgConfig::GDK_PIXBUF
        )
        target_compile_options(realmheart-wallpaper-renderer PRIVATE ${REALMHEART_WARNING_FLAGS})
        set(REALMHEART_NATIVE_WALLPAPER_BUILT ON)
    else()
        message(STATUS "Native wallpaper renderer disabled: one or more optional Wayland/EGL dependencies are unavailable")
    endif()
endif()

install(TARGETS realmheart RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(REALMHEART_NATIVE_WALLPAPER_BUILT)
    install(TARGETS realmheart-wallpaper-renderer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/realmheart/assets)
install(DIRECTORY styles/ DESTINATION ${CMAKE_INSTALL_DATADIR}/realmheart/styles)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)

    function(realmheart_configure_test target)
        target_include_directories(${target} PRIVATE src third_party)
        target_compile_options(${target} PRIVATE ${REALMHEART_WARNING_FLAGS})
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endfunction()

    function(realmheart_register_test target)
        realmheart_configure_test(${target})
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 20)
    endfunction()

    function(realmheart_register_dbus_test target)
        realmheart_configure_test(${target})
        if(DBUS_RUN_SESSION_EXECUTABLE)
            add_test(NAME ${target} COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:${target}>)
        else()
            add_test(NAME ${target} COMMAND ${target})
        endif()
        set_tests_properties(${target} PROPERTIES TIMEOUT 20)
    endfunction()

    add_executable(realmheart_command_tests tests/CommandTests.cpp src/core/Command.cpp)
    realmheart_register_test(realmheart_command_tests)

    add_executable(realmheart_shell_command_tests tests/ShellCommandTests.cpp src/core/ShellCommand.cpp)
    realmheart_register_test(realmheart_shell_command_tests)

    add_executable(realmheart_shell_runtime_tests tests/ShellRuntimeTests.cpp src/core/ShellCommand.cpp src/core/ShellControl.cpp)
    target_link_libraries(realmheart_shell_runtime_tests PRIVATE PkgConfig::GIO)
    realmheart_register_dbus_test(realmheart_shell_runtime_tests)

    add_executable(realmheart_audio_tests tests/AudioTests.cpp src/core/Command.cpp src/services/Audio.cpp)
    realmheart_register_test(realmheart_audio_tests)

    add_executable(realmheart_bar_services_tests tests/BarServicesTests.cpp src/services/BatteryService.cpp src/services/MediaService.cpp)
    target_link_libraries(realmheart_bar_services_tests PRIVATE PkgConfig::GIO)
    realmheart_register_dbus_test(realmheart_bar_services_tests)

    add_executable(realmheart_battery_service_tests tests/BatteryServiceTests.cpp src/services/BatteryService.cpp)
    realmheart_register_test(realmheart_battery_service_tests)

    add_executable(realmheart_bluetooth_tests tests/BluetoothTests.cpp src/core/Command.cpp src/services/Bluetooth.cpp)
    realmheart_register_test(realmheart_bluetooth_tests)

    add_executable(realmheart_brightness_tests tests/BrightnessTests.cpp src/core/Command.cpp src/services/Brightness.cpp)
    realmheart_register_test(realmheart_brightness_tests)

    add_executable(realmheart_gamemode_tests tests/GameModeTests.cpp src/core/Command.cpp src/services/GameMode.cpp)
    realmheart_register_test(realmheart_gamemode_tests)

    add_executable(realmheart_workspace_tests tests/HyprlandWorkspacesTests.cpp src/core/Command.cpp src/services/HyprlandWorkspaces.cpp)
    realmheart_register_test(realmheart_workspace_tests)

    add_executable(realmheart_hyprland_event_monitor_tests tests/HyprlandEventMonitorTests.cpp src/services/HyprlandEventMonitor.cpp)
    realmheart_register_test(realmheart_hyprland_event_monitor_tests)

    add_executable(realmheart_keep_awake_tests tests/KeepAwakeTests.cpp src/core/Command.cpp src/services/KeepAwake.cpp)
    realmheart_register_test(realmheart_keep_awake_tests)

    add_executable(realmheart_launcher_service_command_tests tests/LauncherServiceCommandTests.cpp src/core/Command.cpp src/services/LauncherService.cpp)
    target_link_libraries(realmheart_launcher_service_command_tests PRIVATE GTest::gtest GTest::gtest_main PkgConfig::GIO)
    realmheart_register_test(realmheart_launcher_service_command_tests)

    add_executable(realmheart_launcher_service_tests tests/LauncherServiceTests.cpp src/core/Command.cpp src/services/LauncherService.cpp)
    target_link_libraries(realmheart_launcher_service_tests PRIVATE GTest::gtest GTest::gtest_main PkgConfig::GIO)
    realmheart_register_test(realmheart_launcher_service_tests)

    add_executable(realmheart_matugen_parser_tests tests/MatugenParserTests.cpp src/services/MatugenParser.cpp src/services/ThemeService.cpp)
    realmheart_register_test(realmheart_matugen_parser_tests)

    add_executable(realmheart_night_light_tests tests/NightLightTests.cpp src/core/Command.cpp src/services/NightLight.cpp src/services/SessionManager.cpp)
    realmheart_register_test(realmheart_night_light_tests)

    add_executable(realmheart_notes_tests tests/NotesServiceTests.cpp src/services/NotesService.cpp)
    realmheart_register_test(realmheart_notes_tests)

    add_executable(realmheart_notification_tests tests/NotificationsTests.cpp src/services/Notifications.cpp)
    realmheart_register_test(realmheart_notification_tests)

    add_executable(realmheart_notification_server_tests tests/NotificationServerTests.cpp src/services/NotificationServer.cpp src/services/Notifications.cpp)
    realmheart_register_test(realmheart_notification_server_tests)

    add_executable(realmheart_notification_daemon_tests tests/NotificationDaemonTests.cpp src/services/NotificationServer.cpp src/services/Notifications.cpp src/services/NotificationDaemon.cpp)
    target_link_libraries(realmheart_notification_daemon_tests PRIVATE PkgConfig::GIO)
    realmheart_register_dbus_test(realmheart_notification_daemon_tests)

    add_executable(realmheart_power_profiles_tests tests/PowerProfilesTests.cpp src/core/Command.cpp src/services/PowerProfiles.cpp)
    realmheart_register_test(realmheart_power_profiles_tests)

    add_executable(realmheart_session_manager_tests tests/SessionManagerTests.cpp src/core/Command.cpp src/services/SessionManager.cpp)
    realmheart_register_test(realmheart_session_manager_tests)

    add_executable(realmheart_shell_state_tests tests/ShellStateTests.cpp)
    realmheart_register_test(realmheart_shell_state_tests)

    add_executable(realmheart_theme_service_tests tests/ThemeServiceTests.cpp src/services/ThemeService.cpp)
    realmheart_register_test(realmheart_theme_service_tests)

    add_executable(realmheart_ui_foundation_tests
        tests/UiFoundationTests.cpp
        src/ui/AssetResolver.cpp
        src/ui/ImageFileFilters.cpp
        src/ui/LayerSurface.cpp
        src/ui/styles/CssModuleLoader.cpp
    )
    target_compile_definitions(realmheart_ui_foundation_tests PRIVATE
        REALMHEART_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/Realmheart-Icons"
        REALMHEART_INSTALL_ASSET_DIR="${CMAKE_INSTALL_FULL_DATADIR}/realmheart/assets"
        REALMHEART_SOURCE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
        REALMHEART_INSTALL_STYLE_DIR="${CMAKE_INSTALL_FULL_DATADIR}/realmheart/styles"
        REALMHEART_SOURCE_STYLE_DIR="${CMAKE_SOURCE_DIR}/styles"
    )
    target_link_libraries(realmheart_ui_foundation_tests PRIVATE PkgConfig::GTK4 PkgConfig::GTK4_LAYER_SHELL)
    realmheart_register_test(realmheart_ui_foundation_tests)

    add_executable(realmheart_utility_manager_tests
        tests/UtilityManagerTests.cpp
        src/core/Command.cpp
        src/services/UtilityManager.cpp
        src/services/WallpaperService.cpp
        src/services/ThemeService.cpp
        src/services/MatugenParser.cpp
    )
    realmheart_register_test(realmheart_utility_manager_tests)

    add_executable(realmheart_vertical_bar_model_tests tests/VerticalBarModelTests.cpp src/ui/bar/VerticalBarModel.cpp)
    realmheart_register_test(realmheart_vertical_bar_model_tests)

    add_executable(realmheart_workspace_window_tracker_tests
        tests/WorkspaceWindowTrackerTests.cpp
        src/ui/bar/WorkspaceWindowTracker.cpp
    )
    realmheart_register_test(realmheart_workspace_window_tracker_tests)

    add_executable(realmheart_system_monitor_service_tests
        tests/SystemMonitorServiceTests.cpp
        src/services/SystemMonitorService.cpp
    )
    realmheart_register_test(realmheart_system_monitor_service_tests)

    add_executable(realmheart_media_art_loader_tests
        tests/MediaArtLoaderTests.cpp
        src/core/Command.cpp
        src/ui/bar/MediaArtLoader.cpp
    )
    realmheart_register_test(realmheart_media_art_loader_tests)

    add_executable(realmheart_wallpaper_backend_tests tests/WallpaperBackendTests.cpp src/ui/wallpaper/WallpaperBackend.cpp)
    realmheart_register_test(realmheart_wallpaper_backend_tests)

    add_executable(realmheart_wallpaper_service_tests tests/WallpaperServiceTests.cpp src/services/WallpaperService.cpp src/core/Command.cpp)
    target_link_libraries(realmheart_wallpaper_service_tests PRIVATE GTest::gtest GTest::gtest_main)
    realmheart_register_test(realmheart_wallpaper_service_tests)

    add_executable(realmheart_wifi_tests tests/WifiTests.cpp src/core/Command.cpp src/services/Wifi.cpp)
    realmheart_register_test(realmheart_wifi_tests)
endif()
