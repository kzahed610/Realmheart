#!/usr/bin/env python3
"""Test Realmheart in an isolated nested Hyprland display tier.

Two modes are available:

* capture mode (default) creates deterministic screenshots of selected surfaces;
* ``--interactive`` opens the full isolated 1080p/1440p/4K desktop in a
  scaled GTK-VNC window with real mouse/keyboard input.

The harness never changes the host monitor configuration. The target logical
resolution exists only on an Aquamarine headless output inside a separate
Hyprland instance.

Examples:
    python tools/display-tests/isolated_hyprland.py 1440p --surface sidebar
    python tools/display-tests/isolated_hyprland.py 4k --surface all
    python tools/display-tests/isolated_hyprland.py 1440p --interactive
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TIERS: dict[str, tuple[int, int]] = {
    "1080p": (1920, 1080),
    "1440p": (2560, 1440),
    "4k": (3840, 2160),
}
SURFACES = ("bar", "sidebar", "overview", "notes", "all")
HEADLESS_OUTPUT = "REALMHEART-TEST"


class HarnessError(RuntimeError):
    pass


@dataclass
class HostSnapshot:
    workspace: dict[str, Any]
    active_window: dict[str, Any]
    monitors: list[dict[str, Any]]


def command_path(name: str, *fallbacks: str) -> str:
    for candidate in (name, *fallbacks):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise HarnessError(f"required command is missing: {name}")


def run(
    argv: Iterable[str | os.PathLike[str]],
    *,
    check: bool = True,
    capture: bool = True,
    env: dict[str, str] | None = None,
    timeout: float | None = 15.0,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(item) for item in argv],
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        env=env,
        timeout=timeout,
    )
    if check and result.returncode != 0:
        stdout = result.stdout or ""
        stderr = result.stderr or ""
        raise HarnessError(
            f"command failed ({result.returncode}): {' '.join(map(str, argv))}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )
    return result


def hyprctl_json(hyprctl: str, *args: str, instance: str | None = None) -> Any:
    argv = [hyprctl, "-j"]
    if instance:
        argv += ["-i", instance]
    argv += list(args)
    result = run(argv)
    try:
        return json.loads(result.stdout or "null")
    except json.JSONDecodeError as error:
        raise HarnessError(f"invalid hyprctl JSON for {' '.join(args)}: {error}") from error


def hyprctl_cmd(hyprctl: str, *args: str, instance: str | None = None) -> str:
    argv = [hyprctl]
    if instance:
        argv += ["-i", instance]
    argv += list(args)
    return (run(argv).stdout or "").strip()


def lua_string(value: str) -> str:
    # JSON string escaping is valid for the ASCII paths/selectors emitted here
    # and avoids hand-built quoting inside hyprctl's Lua dispatcher syntax.
    return json.dumps(value)


def hypr_dispatch(
    hyprctl: str,
    lua_dispatcher: str,
    legacy_args: list[str],
    *,
    instance: str | None = None,
) -> str:
    """Dispatch across both the Hyprland 0.55+ Lua IPC and legacy parser.

    Hyprland 0.55 changed ``hyprctl dispatch`` to accept Lua dispatcher
    expressions. Keep a legacy fallback so the harness remains usable with the
    minimum Realmheart-supported branch while users transition configs.
    """
    try:
        return hyprctl_cmd(hyprctl, "dispatch", lua_dispatcher, instance=instance)
    except HarnessError:
        return hyprctl_cmd(hyprctl, "dispatch", *legacy_args, instance=instance)


def set_monitor(
    hyprctl: str,
    *,
    instance: str,
    name: str,
    mode: str | None = None,
    position: str = "0x0",
    scale: float = 1.0,
    disabled: bool = False,
) -> None:
    if disabled:
        modern = f"hl.monitor({{ output = {lua_string(name)}, disabled = true }})"
        legacy = f"{name},disable"
    else:
        if mode is None:
            raise HarnessError("monitor mode is required when enabling an output")
        modern = (
            "hl.monitor({ output = " + lua_string(name) +
            ", mode = " + lua_string(mode) +
            ", position = " + lua_string(position) +
            f", scale = {scale:g} }})"
        )
        legacy = f"{name},{mode},{position},{scale:g}"

    try:
        hyprctl_cmd(hyprctl, "eval", modern, instance=instance)
    except HarnessError:
        hyprctl_cmd(hyprctl, "keyword", "monitor", legacy, instance=instance)


def focus_monitor(hyprctl: str, instance: str, name: str) -> None:
    hypr_dispatch(
        hyprctl,
        f"hl.dsp.focus({{ monitor = {lua_string(name)} }})",
        ["focusmonitor", name],
        instance=instance,
    )


def exit_compositor(hyprctl: str, instance: str) -> None:
    hypr_dispatch(hyprctl, "hl.dsp.exit()", ["exit"], instance=instance)


def dispatch_exec(hyprctl: str, instance: str, argv: list[str]) -> None:
    shell_command = shlex.join(argv)
    hypr_dispatch(
        hyprctl,
        f"hl.dsp.exec_cmd({lua_string(shell_command)})",
        ["--", "exec", *argv],
        instance=instance,
    )


def snapshot_host(hyprctl: str) -> HostSnapshot:
    return HostSnapshot(
        workspace=hyprctl_json(hyprctl, "activeworkspace"),
        active_window=hyprctl_json(hyprctl, "activewindow"),
        monitors=hyprctl_json(hyprctl, "monitors"),
    )


def monitor_signature(monitors: list[dict[str, Any]]) -> list[tuple[Any, ...]]:
    result: list[tuple[Any, ...]] = []
    for monitor in monitors:
        result.append(
            (
                monitor.get("name"),
                monitor.get("width"),
                monitor.get("height"),
                monitor.get("x"),
                monitor.get("y"),
                monitor.get("scale"),
                monitor.get("transform"),
            )
        )
    return sorted(result)


def restore_host(hyprctl: str, snapshot: HostSnapshot) -> list[str]:
    warnings: list[str] = []
    workspace_id = snapshot.workspace.get("id")
    if isinstance(workspace_id, int) and workspace_id > 0:
        try:
            hypr_dispatch(
                hyprctl,
                f"hl.dsp.focus({{ workspace = {workspace_id} }})",
                ["workspace", str(workspace_id)],
            )
        except HarnessError as error:
            warnings.append(f"could not restore host workspace: {error}")

    address = snapshot.active_window.get("address")
    if isinstance(address, str) and address and address != "0x0":
        selector = f"address:{address}"
        try:
            hypr_dispatch(
                hyprctl,
                f"hl.dsp.focus({{ window = {lua_string(selector)} }})",
                ["focuswindow", selector],
            )
        except HarnessError as error:
            warnings.append(f"could not restore host active window: {error}")

    try:
        current = hyprctl_json(hyprctl, "monitors")
        if monitor_signature(current) != monitor_signature(snapshot.monitors):
            warnings.append("host monitor topology differs from the pre-test snapshot")
    except HarnessError as error:
        warnings.append(f"could not verify host monitor topology: {error}")
    return warnings


def interactive_command(realmheart: Path, command: str) -> str:
    return shlex.join([str(realmheart), "--command", command])


def write_minimal_config(path: Path, realmheart: Path) -> None:
    """Write a modern isolated Lua config with test-only convenience binds."""
    binds = {
        "SUPER + S": "sidebar-right-toggle",
        "SUPER + O": "workspace-overview-toggle",
        "SUPER + N": "toggle-notes",
        "SUPER + M": "mana-cores-toggle",
        "SUPER + SPACE": "launch-launcher",
    }

    lines = [
        "-- Generated by Realmheart isolated display testing. No user config is sourced.",
        'hl.monitor({ output = "", mode = "1280x720@60", position = "0x0", scale = 1 })',
        "hl.config({",
        "  animations = { enabled = false },",
        "  misc = {",
        "    disable_hyprland_logo = true,",
        "    disable_splash_rendering = true,",
        "    disable_watchdog_warning = true,",
        "    disable_xdg_env_checks = true,",
        "    disable_hyprland_guiutils_check = true,",
        "    force_default_wallpaper = 0,",
        "  },",
        "})",
    ]
    for keys, command in binds.items():
        shell_command = interactive_command(realmheart, command)
        lines.append(
            f"hl.bind({lua_string(keys)}, hl.dsp.exec_cmd({lua_string(shell_command)}))"
        )
    lines.append('hl.bind("SUPER + SHIFT + Q", hl.dsp.exit())')
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def wait_for_new_instance(
    hyprctl: str,
    existing: set[str],
    process: subprocess.Popen[Any],
    timeout: float = 12.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise HarnessError(f"nested Hyprland exited early with status {process.returncode}")
        instances = hyprctl_json(hyprctl, "instances")
        new_instances = [
            item for item in instances
            if isinstance(item, dict) and item.get("instance") not in existing
        ]
        if new_instances:
            return max(new_instances, key=lambda item: int(item.get("time", 0)))
        time.sleep(0.15)
    raise HarnessError("timed out waiting for the isolated Hyprland instance")


def move_nested_host_window_away(hyprctl: str, nested_pid: int) -> None:
    # The Wayland backend creates one host window. Move it silently to a named
    # special workspace so an interactive VNC viewer is the only test window the
    # user needs to see. Failure here is non-fatal and never changes monitors.
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        clients = hyprctl_json(hyprctl, "clients")
        for client in clients:
            if int(client.get("pid", -1)) != nested_pid:
                continue
            address = client.get("address")
            if isinstance(address, str) and address:
                selector = f"address:{address}"
                hypr_dispatch(
                    hyprctl,
                    "hl.dsp.window.move({ window = " + lua_string(selector) +
                    ', workspace = "special:realmheart-display-test", follow = false })',
                    ["movetoworkspacesilent", f"special:realmheart-display-test,{selector}"],
                )
                return
        time.sleep(0.1)


def configure_headless_output(
    hyprctl: str,
    instance: str,
    width: int,
    height: int,
) -> None:
    hyprctl_cmd(hyprctl, "output", "create", "headless", HEADLESS_OUTPUT, instance=instance)

    # Put the new output beside the nested Wayland-window output first. This
    # avoids Hyprland's overlap error overlay appearing in deterministic captures
    # during the short interval before the bootstrap output is disabled.
    bootstrap_monitors = hyprctl_json(hyprctl, "monitors", instance=instance)
    safe_x = 1280
    for monitor in bootstrap_monitors:
        if monitor.get("name") == HEADLESS_OUTPUT:
            continue
        try:
            x = int(monitor.get("x", 0))
            w = int(monitor.get("width", 0))
            scale = float(monitor.get("scale", 1.0)) or 1.0
            safe_x = max(safe_x, x + int(round(w / scale)) + 64)
        except (TypeError, ValueError):
            pass

    set_monitor(
        hyprctl,
        instance=instance,
        name=HEADLESS_OUTPUT,
        mode=f"{width}x{height}@60",
        position=f"{safe_x}x0",
        scale=1.0,
    )

    deadline = time.monotonic() + 5.0
    monitors: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        monitors = hyprctl_json(hyprctl, "monitors", instance=instance)
        target = next((m for m in monitors if m.get("name") == HEADLESS_OUTPUT), None)
        if target and target.get("width") == width and target.get("height") == height:
            break
        time.sleep(0.15)
    else:
        raise HarnessError(
            f"headless output did not settle at {width}x{height}; monitors={monitors}"
        )

    for monitor in monitors:
        name = monitor.get("name")
        if isinstance(name, str) and name and name != HEADLESS_OUTPUT:
            set_monitor(hyprctl, instance=instance, name=name, disabled=True)

    # Once the bootstrap output is gone, return the test monitor to origin so
    # all layer-shell geometry is expressed in the simplest one-output layout.
    set_monitor(
        hyprctl,
        instance=instance,
        name=HEADLESS_OUTPUT,
        mode=f"{width}x{height}@60",
        position="0x0",
        scale=1.0,
    )
    focus_monitor(hyprctl, instance, HEADLESS_OUTPUT)

    final = hyprctl_json(hyprctl, "monitors", instance=instance)
    active = [m for m in final if m.get("name") == HEADLESS_OUTPUT]
    if len(final) != 1 or len(active) != 1:
        raise HarnessError(f"isolated compositor did not converge to one output: {final}")


def wait_for_file(path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and path.stat().st_size > 0:
            return
        time.sleep(0.1)
    raise HarnessError(f"timed out waiting for screenshot: {path}")


def capture(
    hyprctl: str,
    instance: str,
    grim: str,
    destination: Path,
) -> None:
    if destination.exists():
        destination.unlink()
    dispatch_exec(hyprctl, instance, [grim, "-o", HEADLESS_OUTPUT, str(destination)])
    wait_for_file(destination)


def realmheart_env_prefix(repo_root: Path, session_root: Path) -> list[str]:
    return [
        "env",
        f"REALMHEART_ASSET_DIR={repo_root / 'assets'}",
        f"REALMHEART_STYLE_DIR={repo_root / 'styles'}",
        f"REALMHEART_EFFECT_DIR={repo_root / 'effects'}",
        f"REALMHEART_THEME_CACHE={session_root / 'state' / 'theme-palette.tsv'}",
        "REALMHEART_WALLPAPER_BACKEND=gtk",
    ]


def capture_simple_surface(
    surface: str,
    *,
    hyprctl: str,
    instance: str,
    grim: str,
    realmheart: Path,
    repo_root: Path,
    session_root: Path,
    destination: Path,
) -> None:
    command = realmheart_env_prefix(repo_root, session_root)
    command += [str(realmheart), f"--{surface}", "--timeout", "5"]
    dispatch_exec(hyprctl, instance, command)
    time.sleep(1.4)
    capture(hyprctl, instance, grim, destination)
    time.sleep(4.0)


def capture_shell_surface(
    surface: str,
    *,
    hyprctl: str,
    instance: str,
    grim: str,
    realmheart: Path,
    repo_root: Path,
    session_root: Path,
    destination: Path,
) -> None:
    shell = realmheart_env_prefix(repo_root, session_root)
    shell += [str(realmheart), "--shell", "--wallpaper-backend", "gtk"]
    dispatch_exec(hyprctl, instance, shell)
    time.sleep(2.5)

    actions = {
        "sidebar": "sidebar-right-toggle",
        "overview": "workspace-overview-toggle",
        "notes": "toggle-notes",
    }
    try:
        action = actions[surface]
    except KeyError as error:
        raise HarnessError(f"unsupported integrated shell capture: {surface}") from error

    dispatch_exec(
        hyprctl,
        instance,
        realmheart_env_prefix(repo_root, session_root) +
        [str(realmheart), "--command", action],
    )
    time.sleep(1.3)
    capture(hyprctl, instance, grim, destination)

    dispatch_exec(
        hyprctl,
        instance,
        realmheart_env_prefix(repo_root, session_root) +
        [str(realmheart), "--command", "quit"],
    )
    time.sleep(1.0)


def find_vnc_port() -> tuple[int, int]:
    for display in range(21, 100):
        port = 5900 + display
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
        return display, port
    raise HarnessError("could not find a free localhost VNC port in 5921-5999")


def wait_for_tcp(port: int, process: subprocess.Popen[Any], timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise HarnessError(f"wayvnc exited early with status {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.15):
                return
        except OSError:
            time.sleep(0.1)
    raise HarnessError(f"timed out waiting for wayvnc on localhost:{port}")


def nested_wayland_env(base: dict[str, str], instance_data: dict[str, Any]) -> dict[str, str]:
    env = base.copy()
    instance = str(instance_data.get("instance", ""))
    wl_socket = instance_data.get("wl_socket")
    if not instance:
        raise HarnessError("nested Hyprland instance has no instance signature")
    if not isinstance(wl_socket, str) or not wl_socket:
        # Never let wayvnc silently inherit the host WAYLAND_DISPLAY. If the
        # nested compositor cannot tell us its own Wayland socket, abort instead
        # of risking an interactive server against the production desktop.
        raise HarnessError("nested Hyprland instance did not expose a Wayland socket")

    env["HYPRLAND_INSTANCE_SIGNATURE"] = instance
    env["WAYLAND_DISPLAY"] = wl_socket
    return env


def run_scaled_vnc_viewer(port: int, tier: str, host: HostSnapshot) -> None:
    try:
        import gi  # type: ignore
        gi.require_version("Gtk", "3.0")
        gi.require_version("GtkVnc", "2.0")
        from gi.repository import Gtk, GtkVnc  # type: ignore
    except (ImportError, ValueError) as error:
        raise HarnessError(
            "interactive mode needs GTK-VNC Python bindings. On Arch/CachyOS: "
            "sudo pacman -S --needed wayvnc gtk-vnc python-gobject"
        ) from error

    focused = next((m for m in host.monitors if m.get("focused")), None)
    host_width = 1920
    host_height = 1080
    if focused is not None:
        try:
            scale = float(focused.get("scale", 1.0)) or 1.0
            host_width = int(round(int(focused.get("width", 1920)) / scale))
            host_height = int(round(int(focused.get("height", 1080)) / scale))
        except (TypeError, ValueError):
            pass

    # Keep enough host chrome visible to close/move the viewer while giving the
    # scaled 16:9 framebuffer almost the whole current monitor.
    viewer_width = max(960, min(1728, host_width - 120))
    viewer_height = max(540, min(972, int(round(viewer_width * 9 / 16))))
    if viewer_height > host_height - 120:
        viewer_height = max(540, host_height - 120)
        viewer_width = int(round(viewer_height * 16 / 9))

    window = Gtk.Window(title=f"Realmheart {tier} isolated interactive test")
    window.set_default_size(viewer_width, viewer_height)

    display = GtkVnc.Display()
    display.set_scaling(True)
    display.set_keep_aspect_ratio(True)
    display.set_smoothing(True)
    display.set_allow_resize(False)  # Never resize the target logical monitor.
    display.set_read_only(False)
    display.set_keyboard_grab(True)
    display.set_pointer_grab(True)
    display.set_pointer_local(True)

    window.add(display)
    window.connect("destroy", lambda *_: Gtk.main_quit())
    display.connect("vnc-disconnected", lambda *_: Gtk.main_quit())
    display.connect("vnc-initialized", lambda *_: display.set_scaling(True))
    window.show_all()

    if not display.open_host("127.0.0.1", str(port)):
        window.destroy()
        raise HarnessError(f"GTK-VNC could not open localhost:{port}")

    Gtk.main()


def run_interactive_session(
    *,
    tier: str,
    hyprctl: str,
    instance: str,
    instance_data: dict[str, Any],
    wayvnc: str,
    realmheart: Path,
    repo_root: Path,
    session_root: Path,
    output_dir: Path,
    nested_base_env: dict[str, str],
    host: HostSnapshot,
) -> None:
    shell = realmheart_env_prefix(repo_root, session_root)
    shell += [str(realmheart), "--shell", "--wallpaper-backend", "gtk"]
    dispatch_exec(hyprctl, instance, shell)
    time.sleep(2.5)

    display_number, port = find_vnc_port()
    vnc_log_path = output_dir / f"wayvnc-{tier}.log"
    vnc_log = vnc_log_path.open("w", encoding="utf-8")
    vnc_process: subprocess.Popen[Any] | None = None
    try:
        wayland_env = nested_wayland_env(nested_base_env, instance_data)
        vnc_process = subprocess.Popen(
            [wayvnc, "-o", HEADLESS_OUTPUT, "127.0.0.1", str(port)],
            stdout=vnc_log,
            stderr=subprocess.STDOUT,
            env=wayland_env,
        )
        wait_for_tcp(port, vnc_process)

        print("\n[interactive] full Realmheart shell is live")
        print(f"[interactive] framebuffer: {TIERS[tier][0]}x{TIERS[tier][1]} logical @ scale 1")
        print(f"[interactive] VNC: localhost:{port} (display :{display_number})")
        print("[interactive] shortcuts inside the viewer:")
        print("  Super+S       right sidebar")
        print("  Super+O       workspace overview")
        print("  Super+N       notes")
        print("  Super+M       wallpaper selector")
        print("  Super+Space   launcher")
        print("  Super+Shift+Q end isolated compositor")
        print("Close the viewer window when you are done.\n")

        run_scaled_vnc_viewer(port, tier, host)
    finally:
        try:
            dispatch_exec(
                hyprctl,
                instance,
                realmheart_env_prefix(repo_root, session_root) +
                [str(realmheart), "--command", "quit"],
            )
        except Exception:
            pass
        if vnc_process is not None and vnc_process.poll() is None:
            vnc_process.terminate()
            try:
                vnc_process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                vnc_process.kill()
        vnc_log.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tier", choices=TIERS)
    parser.add_argument("--surface", choices=SURFACES, default="all")
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="open a scaled, input-capable GTK-VNC session instead of taking screenshots",
    )
    parser.add_argument(
        "--realmheart",
        type=Path,
        help="Realmheart executable (default: <repo>/build-hybrid/realmheart)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="artifact directory (default: /tmp/realmheart-display-tests/...)",
    )
    parser.add_argument(
        "--keep-session",
        action="store_true",
        help="keep the generated isolated config/log/state directory",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    realmheart = (args.realmheart or (repo_root / "build-hybrid" / "realmheart")).resolve()
    if not realmheart.is_file() or not os.access(realmheart, os.X_OK):
        raise HarnessError(f"Realmheart executable is missing or not executable: {realmheart}")

    hyprctl = command_path("hyprctl")
    hyprland = command_path("Hyprland", "hyprland")
    dbus_run_session = command_path("dbus-run-session")
    grim = None if args.interactive else command_path("grim")
    wayvnc = command_path("wayvnc") if args.interactive else None

    width, height = TIERS[args.tier]
    stamp = time.strftime("%Y%m%d-%H%M%S")
    output_dir = (
        args.output_dir or Path("/tmp/realmheart-display-tests") / f"{stamp}-{args.tier}"
    ).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    session_root = Path(tempfile.mkdtemp(prefix=f"realmheart-{args.tier}-", dir="/tmp"))
    config_path = session_root / "hyprland.lua"
    log_path = output_dir / f"hyprland-{args.tier}.log"
    for subdir in ("config", "state", "cache", "data"):
        (session_root / subdir).mkdir(parents=True, exist_ok=True)
    write_minimal_config(config_path, realmheart)

    host = snapshot_host(hyprctl)
    existing_instances = {
        str(item.get("instance"))
        for item in hyprctl_json(hyprctl, "instances")
        if isinstance(item, dict) and item.get("instance")
    }

    env = os.environ.copy()
    env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
    env.pop("HYPRLAND_CMD", None)
    env.update({
        "HYPRLAND_NO_SD_VARS": "1",
        "HYPRLAND_NO_SD_NOTIFY": "1",
        "HYPRLAND_NO_RT": "1",
        "XDG_CONFIG_HOME": str(session_root / "config"),
        "XDG_STATE_HOME": str(session_root / "state"),
        "XDG_CACHE_HOME": str(session_root / "cache"),
        "XDG_DATA_HOME": str(session_root / "data"),
        "REALMHEART_ASSET_DIR": str(repo_root / "assets"),
        "REALMHEART_STYLE_DIR": str(repo_root / "styles"),
        "REALMHEART_EFFECT_DIR": str(repo_root / "effects"),
        "REALMHEART_WALLPAPER_BACKEND": "gtk",
    })

    print(f"[session] isolated state: {session_root}")
    print(f"[session] Hyprland log: {log_path}")

    nested: subprocess.Popen[Any] | None = None
    log_stream = log_path.open("w", encoding="utf-8")
    warnings: list[str] = []
    screenshot_paths: list[Path] = []
    instance = ""

    try:
        nested = subprocess.Popen(
            [dbus_run_session, "--", hyprland, "-c", str(config_path)],
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        instance_data = wait_for_new_instance(hyprctl, existing_instances, nested)
        instance = str(instance_data["instance"])
        nested_pid = int(instance_data.get("pid", 0))
        if nested_pid > 0:
            try:
                move_nested_host_window_away(hyprctl, nested_pid)
            except HarnessError as error:
                warnings.append(f"could not hide nested host window: {error}")

        configure_headless_output(hyprctl, instance, width, height)

        if args.interactive:
            assert wayvnc is not None
            run_interactive_session(
                tier=args.tier,
                hyprctl=hyprctl,
                instance=instance,
                instance_data=instance_data,
                wayvnc=wayvnc,
                realmheart=realmheart,
                repo_root=repo_root,
                session_root=session_root,
                output_dir=output_dir,
                nested_base_env=env,
                host=host,
            )
        else:
            assert grim is not None
            selected = ["bar", "sidebar", "overview", "notes"] if args.surface == "all" else [args.surface]
            for surface in selected:
                destination = output_dir / f"realmheart-{args.tier}-{surface}.png"
                print(f"[capture] {surface} @ {width}x{height}")
                if surface == "bar":
                    capture_simple_surface(
                        surface,
                        hyprctl=hyprctl,
                        instance=instance,
                        grim=grim,
                        realmheart=realmheart,
                        repo_root=repo_root,
                        session_root=session_root,
                        destination=destination,
                    )
                else:
                    # Sidebar/overview/notes are captured through the real shell
                    # and its command surface, not standalone MVP windows. This
                    # catches the same monitor routing, backdrop and compositor
                    # lifecycle the user actually sees.
                    capture_shell_surface(
                        surface,
                        hyprctl=hyprctl,
                        instance=instance,
                        grim=grim,
                        realmheart=realmheart,
                        repo_root=repo_root,
                        session_root=session_root,
                        destination=destination,
                    )
                screenshot_paths.append(destination)

    finally:
        if instance:
            try:
                exit_compositor(hyprctl, instance)
            except Exception:
                pass
        if nested is not None:
            try:
                nested.wait(timeout=4.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(nested.pid, signal.SIGTERM)
                    nested.wait(timeout=2.0)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    try:
                        os.killpg(nested.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        log_stream.close()
        warnings.extend(restore_host(hyprctl, host))

    if args.interactive:
        print("\nIsolated Realmheart interactive session complete.")
        print(f"Hyprland log: {log_path}")
    else:
        print("\nIsolated Realmheart display capture complete:")
        for path in screenshot_paths:
            print(f"  {path}")
        print(f"Hyprland log: {log_path}")

    if warnings:
        print("Warnings:")
        for warning in warnings:
            print(f"  - {warning}")

    if not args.keep_session and not warnings:
        shutil.rmtree(session_root, ignore_errors=True)
    elif args.keep_session or warnings:
        print(f"Session data retained: {session_root}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HarnessError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
