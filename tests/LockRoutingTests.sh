#!/usr/bin/env bash
set -euo pipefail

root="${1:?usage: LockRoutingTests.sh SOURCE_DIR}"
hypridle="$root/config/hypr/hypridle.conf"
power_renderer="$root/src/powermenu-renderer/main.cpp"
power_overlay="$root/src/ui/powermenu/PowerMenuOverlay.hpp"
shell_app="$root/src/ui/ShellApp.cpp"
lock_surface_header="$root/src/ui/lockscreen/LockSurface.hpp"
lock_surface_cpp="$root/src/ui/lockscreen/LockSurface.cpp"
shell_control="$root/src/core/ShellControl.cpp"
main_cpp="$root/src/main.cpp"
session_manager="$root/src/services/SessionManager.hpp"
session_impl="$root/src/services/SessionManager.cpp"
cmake_file="$root/CMakeLists.txt"

require_line() {
    local file="$1"
    local expected="$2"
    if ! grep -Fqx -- "$expected" "$file"; then
        printf 'missing expected line in %s: %s\n' "$file" "$expected" >&2
        exit 1
    fi
}

require_absent() {
    local file="$1"
    local forbidden="$2"
    if grep -Fq -- "$forbidden" "$file"; then
        printf 'forbidden lock route in %s: %s\n' "$file" "$forbidden" >&2
        exit 1
    fi
}

require_line "$hypridle" '$lock_cmd = $HOME/.local/bin/realmheart-lock-session'
require_line "$hypridle" '$suspend_cmd = $HOME/.local/bin/realmheart-lock-session --suspend'
require_line "$hypridle" '    lock_cmd = $lock_cmd'
require_line "$hypridle" '    before_sleep_cmd = $lock_cmd'
require_line "$hypridle" '    on-timeout = $lock_cmd'
require_line "$hypridle" '    inhibit_sleep = 3'
require_line "$hypridle" '    # Native Broken Seal owns hyprland-lock-notify-v1 readiness.'
require_absent "$hypridle" '|| /usr/bin/hyprlock'
require_absent "$hypridle" 'loginctl lock-session'

require_line "$power_renderer" '                const auto result = realmheart::core::request_shell_lock();'
require_line "$power_overlay" '    // This is deliberately not a hyprlock process-state probe.'
require_absent "$power_renderer" 'is_locked()'
require_absent "$power_renderer" '/usr/bin/hyprlock'
require_absent "$session_manager" 'is_locked'
require_absent "$session_manager" 'pgrep'
require_line "$session_manager" '    bool fallback_lock();'
require_line "$session_manager" '    [[nodiscard]] std::optional<pid_t> fallback_lock_tracked();'
require_line "$session_manager" '    void request_emergency_lock();'
require_absent "$session_manager" '    bool emergency_lock();'
require_line "$session_impl" 'bool SessionManager::fallback_lock() {'
require_line "$session_impl" 'std::optional<pid_t> SessionManager::fallback_lock_tracked() {'
require_line "$session_impl" 'void SessionManager::request_emergency_lock() {'
require_line "$session_impl" '    static_cast<void>(executor_->run_capture_succeeded_bounded('
require_line "$cmake_file" '    src/core/ShellControl.cpp'
require_line "$cmake_file" '        # Keep CMAKE_INSTALL_PREFIX runtime-resolvable so'
require_line "$cmake_file" '            "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@REALMHEART_AUTH_HELPER_RELATIVE_PATH@"'
require_line "$root/install-hypr-configs.sh" 'install_lock_command() {'
grep -Fq -- 'if [[ ! -x "$binary" ]]; then' "$root/install-hypr-configs.sh"
grep -Fq -- 'if ! "$binary" --command lock-session "$$-$(/usr/bin/date +%s%N)"; then' "$root/install-hypr-configs.sh"
require_absent "$root/install-hypr-configs.sh" 'status_file='
require_absent "$root/install-hypr-configs.sh" '/usr/bin/hyprlock'
require_absent "$root/install-hypr-configs.sh" 'fallback_lock()'
grep -Fq -- '/usr/bin/systemctl suspend || /usr/bin/loginctl suspend' "$root/install-hypr-configs.sh"

require_line "$shell_control" 'ShellControlResult request_shell_lock(std::string_view request_token) {'
require_line "$shell_control" '        "LockSession",'
require_line "$main_cpp" '        if (*shell_command == realmheart::core::ShellCommand::LockSession) {'
require_line "$main_cpp" '            ? realmheart::core::request_shell_lock(shell_argument)'
require_line "$shell_app" 'constexpr int kNativeLockReadyTimeoutMs = 5000;'
require_line "$shell_app" '        lock_failure_terminal_ = true;'
grep -Fq -- 'kNativeLockReadyStatus' "$shell_app"
require_line "$shell_app" '            enter_terminal_lock_failure("native lock and hyprlock fallback are unavailable");'
require_absent "$shell_app" 'constexpr std::string_view kFallbackLockReadyStatus'
require_absent "$shell_app" 'watch->observed_running'
require_line "$shell_app" '                    owner->finish_hyprlock_fallback(status);'
require_line "$shell_app" '                    owner->enter_terminal_lock_failure('
grep -Fq -- 'startup_deadline_us' "$shell_app"
grep -Fq -- 'if (owner->all_lock_surfaces_mapped()) {' "$shell_app"
grep -Fq -- 'const bool terminal_lock_active = lock_failure_terminal_;' "$shell_app"
require_line "$shell_app" '        publish_pending_lock_status(kLockFailureStatus);'
require_line "$shell_app" '                owner->publish_pending_lock_status(kNativeLockReadyStatus);'
require_line "$shell_app" '    void show_terminal_lock_surfaces() {'
require_line "$shell_app" '    void cancel_lock_surface_coverage_watch() {'
require_line "$shell_app" '    void cancel_hyprlock_fallback_watchdog() {'
require_line "$shell_app" '    guint hyprlock_watch_id_ = 0;'
require_line "$shell_app" '    pid_t hyprlock_fallback_pid_ = -1;'
require_line "$shell_app" '        const bool fallback_lock_active ='
require_line "$shell_app" '        } else if (fallback_lock_active) {'
require_line "$shell_app" '                "hyprlock fallback lock surfaces failed to map after monitor hotplug"'
require_line "$shell_app" '    std::vector<std::string> lock_waiting_tokens_;'
require_line "$shell_app" '            remember_lock_request(request_token);'
require_line "$shell_app" '        // documented SIGUSR1 handler exits with status 0 after unlocking.'
# Do not globally ban finish_lock_unlock(): the native Broken Seal success path
# deliberately schedules it on the next GLib main-loop turn so LockSurface's
# animation callback can unwind before session-lock teardown destroys surfaces.
# The security invariant is narrower: the hyprlock fallback watcher must never
# treat child exit/liveness as authenticated unlock completion.
fallback_start=$(grep -nF -- '    void fallback_to_hyprlock(std::string_view reason) {' "$shell_app" | head -n1 | cut -d: -f1)
fallback_end=$(grep -nF -- '    void lock_session(std::string_view request_token = {}) {' "$shell_app" | head -n1 | cut -d: -f1)
if [[ -z "$fallback_start" || -z "$fallback_end" || "$fallback_start" -ge "$fallback_end" ]]; then
    printf 'unable to locate hyprlock fallback routing block in %s\n' "$shell_app" >&2
    exit 1
fi
if sed -n "${fallback_start},$((fallback_end - 1))p" "$shell_app" | grep -Fq -- 'finish_lock_unlock'; then
    printf 'hyprlock fallback must not route process exit/liveness to finish_lock_unlock\n' >&2
    exit 1
fi

# Native unlock completion is intentionally deferred out of LockSurface's
# animation tick. A synchronous finish here can destroy the surface while its
# own advance_frame() call is still on the stack.
native_unlock_line=$(grep -nF -- '        lock_surface_->set_unlocked_callback([async_state = runtime_async_state_] {' "$shell_app" | head -n1 | cut -d: -f1)
if [[ -z "$native_unlock_line" ]]; then
    printf 'missing deferred native unlock callback in %s\n' "$shell_app" >&2
    exit 1
fi
native_unlock_end=$(grep -nF -- '        lock_mirror_surfaces_.reserve(' "$shell_app" | awk -F: -v start="$native_unlock_line" '$1 > start { print $1; exit }')
if [[ -z "$native_unlock_end" || "$native_unlock_line" -ge "$native_unlock_end" ]]; then
    printf 'unable to bound deferred native unlock callback in %s\n' "$shell_app" >&2
    exit 1
fi
native_unlock_block=$(sed -n "${native_unlock_line},$((native_unlock_end - 1))p" "$shell_app")
if ! grep -Fq -- 'g_idle_add_full(' <<<"$native_unlock_block" ||
   ! grep -Fq -- 'owner->finish_lock_unlock();' <<<"$native_unlock_block"; then
    printf 'native unlock completion must remain deferred off the LockSurface animation stack\n' >&2
    exit 1
fi
require_line "$shell_app" '        session_lock_ = gtk_session_lock_instance_new();'
require_line "$shell_app" '        if (!gtk_session_lock_instance_lock(session_lock_)) {'
require_line "$shell_app" '                gtk_session_lock_instance_is_locked(owner->session_lock_)) {'
require_line "$shell_app" '            gtk_session_lock_instance_unlock(session_lock_);'
require_line "$shell_app" '                ? "hyprlock fallback exited without a verifiable authentication result"'
require_line "$shell_app" '            enter_terminal_lock_failure("unable to activate compositor lock submap");'
require_absent "$shell_app" 'publish_native_lock_status('
require_absent "$shell_app" 'session_->emergency_lock()'

require_line "$lock_surface_header" '    [[nodiscard]] bool coverage_verified() const noexcept;' # LockSurface header contract
require_line "$lock_surface_cpp" '                state->monitor_binding_verified = set_layer_surface_monitor('
require_line "$lock_surface_cpp" 'bool LockSurface::coverage_verified() const noexcept {'
require_line "$lock_surface_cpp" '        !state_->authentication_enabled || state_->closing ||'
require_line "$shell_app" '            lock_surface_->set_authentication_enabled(false);'
require_line "$shell_app" '        const bool lock_surfaces_must_hide ='
require_line "$shell_app" '        } else if (native_lock_visible) {'
require_line "$shell_app" '                "monitor topology changed while Broken Seal was active"'
require_line "$shell_app" '                    owner->show_terminal_lock_surfaces();'
require_line "$shell_app" '    application_class->dbus_register = realmheart_application_dbus_register;'
require_line "$shell_app" '    application_class->dbus_unregister = realmheart_application_dbus_unregister;'
require_line "$shell_app" '    context->registration = g_dbus_connection_register_object('
require_absent "$shell_app" 'g_application_get_dbus_connection('

registration_hook_line=$(grep -nF -- 'application_class->dbus_register = realmheart_application_dbus_register;' "$shell_app" | head -n1 | cut -d: -f1)
object_register_line=$(grep -nF -- 'context->registration = g_dbus_connection_register_object(' "$shell_app" | head -n1 | cut -d: -f1)
if [[ -z "$registration_hook_line" || -z "$object_register_line" || "$registration_hook_line" -ge "$object_register_line" ]]; then
    printf 'private lock D-Bus export must be installed by the GApplication dbus_register hook\n' >&2
    exit 1
fi
remote_detection_line=$(grep -nF -- 'g_application_get_is_remote(G_APPLICATION(application))' "$shell_app" | head -n1 | cut -d: -f1)
startup_reset_line=$(grep -nF -- 'refusing to start while compositor bind-map reset failed' "$shell_app" | head -n1 | cut -d: -f1)
if [[ -z "$remote_detection_line" || -z "$startup_reset_line" || "$remote_detection_line" -ge "$startup_reset_line" ]]; then
    printf 'remote shell detection must precede compositor-global startup reset\n' >&2
    exit 1
fi

printf 'Lock routing contract tests passed\n'
