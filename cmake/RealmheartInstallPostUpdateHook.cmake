# Generates the pacman post-transaction hook at install time so an overridden
# install prefix (`cmake --install --prefix ...`) and a custom sysconfdir are
# honored.  The hook only runs the tiny marker script; it never runs the
# Doctor inside the package transaction.
file(READ "@CMAKE_SOURCE_DIR@/config/pacman/hooks/10-realmheart-doctor.hook.in" _realmheart_hook)

if(IS_ABSOLUTE "@CMAKE_INSTALL_SYSCONFDIR@")
    set(_realmheart_hook_dir "@CMAKE_INSTALL_SYSCONFDIR@")
else()
    set(_realmheart_hook_dir "${CMAKE_INSTALL_PREFIX}/@CMAKE_INSTALL_SYSCONFDIR@")
endif()

if(IS_ABSOLUTE "@CMAKE_INSTALL_LIBEXECDIR@")
    set(_realmheart_marker_dir "@CMAKE_INSTALL_LIBEXECDIR@")
else()
    set(_realmheart_marker_dir "${CMAKE_INSTALL_PREFIX}/@CMAKE_INSTALL_LIBEXECDIR@")
endif()

set(_realmheart_marker "${_realmheart_marker_dir}/realmheart/realmheart-post-update-marker")
set(_realmheart_token "REALMHEART_MARKER_EXECUTABLE")
string(REPLACE "@${_realmheart_token}@" "${_realmheart_marker}" _realmheart_hook "${_realmheart_hook}")

file(WRITE "$ENV{DESTDIR}${_realmheart_hook_dir}/pacman.d/hooks/10-realmheart-doctor.hook" "${_realmheart_hook}")
