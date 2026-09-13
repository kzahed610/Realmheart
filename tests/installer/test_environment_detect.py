from __future__ import annotations

import json
import unittest

from . import _bootstrap  # noqa: F401
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.environment.detect import (
    detect_displays,
    detect_distro,
    detect_hyprland,
    detect_package_manager,
    detect_session,
    DistroInfo,
)
from realmheart_installer.environment.support import HyprlandCompatibility


class FakeRunner:
    def __init__(self, *, which: dict[str, str] | None = None, results: dict[tuple[str, ...], CommandResult] | None = None) -> None:
        self.which_map = which or {}
        self.results = results or {}

    def which(self, executable: str) -> str | None:
        return self.which_map.get(executable)

    def run(self, argv, **kwargs) -> CommandResult:
        key = tuple(str(item) for item in argv)
        return self.results.get(key, CommandResult(key, 1, stderr="not mocked"))


class DetectTests(unittest.TestCase):
    def test_os_release_parsing(self) -> None:
        distro = detect_distro(text='ID=cachyos\nID_LIKE="arch linux"\nPRETTY_NAME="CachyOS"\nVERSION_ID=rolling\n')
        self.assertEqual(distro.id, "cachyos")
        self.assertEqual(distro.pretty_name, "CachyOS")
        self.assertEqual(distro.id_like, ("arch", "linux"))

    def test_pacman_is_only_automatic_adapter(self) -> None:
        runner = FakeRunner(which={"pacman": "/usr/bin/pacman", "apt-get": "/usr/bin/apt-get"})
        info = detect_package_manager(runner, distro=DistroInfo(id="arch", pretty_name="Arch Linux"))
        self.assertEqual(info.kind, "pacman")
        self.assertTrue(info.automatic_dependency_install)

        apt = detect_package_manager(FakeRunner(which={"apt-get": "/usr/bin/apt-get"}), distro=DistroInfo(id="debian", pretty_name="Debian"))
        self.assertEqual(apt.kind, "apt")
        self.assertFalse(apt.automatic_dependency_install)

        cursed = detect_package_manager(FakeRunner(which={"pacman": "/usr/bin/pacman"}), distro=DistroInfo(id="debian", pretty_name="Debian"))
        self.assertEqual(cursed.kind, "pacman")
        self.assertFalse(cursed.automatic_dependency_install)

    def test_hyprland_json_version_and_monitors(self) -> None:
        hyprctl = "/usr/bin/hyprctl"
        version_payload = json.dumps({"version": "0.56.2", "branch": "main", "commit": "abc", "abiHash": "abi-xyz", "dirty": False})
        monitors_payload = json.dumps([
            {"name": "eDP-1", "description": "Internal", "width": 1920, "height": 1080, "refreshRate": 60.0, "scale": 1.0, "x": 0, "y": 0, "focused": True, "disabled": False},
            {"name": "HDMI-A-1", "width": 2560, "height": 1440, "refreshRate": 144.0, "scale": 1.25, "x": 1920, "y": 0, "focused": False, "disabled": False},
        ])
        runner = FakeRunner(
            which={"hyprctl": hyprctl},
            results={
                (hyprctl, "version", "-j"): CommandResult((hyprctl, "version", "-j"), 0, version_payload),
                (hyprctl, "monitors", "-j"): CommandResult((hyprctl, "monitors", "-j"), 0, monitors_payload),
            },
        )
        hypr = detect_hyprland(runner)
        self.assertEqual(hypr.compatibility, HyprlandCompatibility.PREFERRED)
        self.assertEqual(str(hypr.version), "0.56.2")
        self.assertEqual(hypr.abi_hash, "abi-xyz")
        displays = detect_displays(runner, hypr)
        self.assertEqual(len(displays), 2)
        self.assertEqual(displays[1].refresh_hz, 144.0)
        self.assertEqual(displays[1].scale, 1.25)

    def test_session_uses_wayland_env_and_systemd_probe(self) -> None:
        systemctl = "/usr/bin/systemctl"
        runner = FakeRunner(
            which={"systemctl": systemctl},
            results={(systemctl, "--user", "show-environment"): CommandResult((systemctl, "--user", "show-environment"), 0, "A=B\n")},
        )
        session = detect_session(
            env={"XDG_SESSION_TYPE": "wayland", "WAYLAND_DISPLAY": "wayland-1", "HYPRLAND_INSTANCE_SIGNATURE": "sig"},
            runner=runner,
        )
        self.assertTrue(session.wayland)
        self.assertTrue(session.hyprland_environment)
        self.assertTrue(session.systemd_user_available)


if __name__ == "__main__":
    unittest.main()
