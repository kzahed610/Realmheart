cmake_minimum_required(VERSION 3.25)
project(Realmheart VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REALMHEART_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GTK4_LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)

add_executable(realmheart
    src/main.cpp
    src/core/Command.cpp
    src/core/Diagnostics.cpp
    src/core/ModuleRegistry.cpp
    src/services/Audio.cpp
    src/services/Brightness.cpp
    src/services/HyprlandWorkspaces.cpp
    src/services/Notifications.cpp
    src/services/PowerProfiles.cpp
    src/services/ScopeModules.cpp
    src/services/RightSidebarServices.cpp
)

target_include_directories(realmheart PRIVATE src)

add_library(realmheart_ui STATIC
    src/ui/AssetResolver.cpp
    src/ui/GtkApp.cpp
    src/ui/LayerSurface.cpp
    src/ui/bar/VerticalBar.cpp
    src/ui/bar/VerticalBarModel.cpp
    src/ui/sidebar/RightSidebar.cpp
    src/ui/lockscreen/LockScreen.cpp
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

target_link_libraries(realmheart PRIVATE realmheart_ui)

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

    add_test(NAME realmheart_doctor COMMAND realmheart --doctor)
    add_test(NAME realmheart_modules COMMAND realmheart --list-modules)
    add_test(NAME realmheart_workspaces COMMAND realmheart --workspace-status)
    add_test(NAME realmheart_command_runner COMMAND realmheart_command_tests)
    add_test(NAME realmheart_workspace_parser COMMAND realmheart_workspace_tests)
    add_test(NAME realmheart_notification_history COMMAND realmheart_notification_tests)
    add_test(NAME realmheart_vertical_bar_model COMMAND realmheart_vertical_bar_model_tests)
    add_test(NAME realmheart_ui_foundation COMMAND realmheart_ui_foundation_tests)
endif()
