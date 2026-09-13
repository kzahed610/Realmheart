from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.environment.capabilities import (
    CapabilityScanner,
    CapabilityState,
    DependencyLifecycle,
    ExecutableSpec,
    PkgConfigSpec,
    RequirementLevel,
)
from realmheart_installer.environment.command import CommandResult


class FakeRunner:
    def __init__(self, *, which: dict[str, str] | None = None, callback=None) -> None:
        self.which_map = which or {}
        self.callback = callback

    def which(self, executable: str) -> str | None:
        return self.which_map.get(executable)

    def run(self, argv, **kwargs) -> CommandResult:
        key = tuple(str(item) for item in argv)
        if self.callback:
            result = self.callback(key)
            if result is not None:
                return result
        return CommandResult(key, 1, stderr="not mocked")


class CapabilityTests(unittest.TestCase):
    def test_executable_minimum_version(self) -> None:
        fish = "/usr/bin/fish"
        runner = FakeRunner(
            which={"fish": fish},
            callback=lambda argv: CommandResult(argv, 0, "fish, version 4.7.1\n") if argv == (fish, "--version") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            spec = ExecutableSpec("runtime.fish", "Fish >= 4.3", "fish", RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), version_argv=("--version",), minimum_version=(4, 3, 0))
            result = scanner.probe_executable(spec)
            self.assertEqual(result.state, CapabilityState.PASS)

    def test_old_executable_version_fails_capability(self) -> None:
        fish = "/usr/bin/fish"
        runner = FakeRunner(
            which={"fish": fish},
            callback=lambda argv: CommandResult(argv, 0, "fish, version 4.0.2\n") if argv == (fish, "--version") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            spec = ExecutableSpec("runtime.fish", "Fish >= 4.3", "fish", RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), version_argv=("--version",), minimum_version=(4, 3, 0))
            result = scanner.probe_executable(spec)
            self.assertEqual(result.state, CapabilityState.FAILED)
            self.assertIn("requires >= 4.3.0", result.detail)

    def test_pkg_config_minimum_version(self) -> None:
        pkg = "/usr/bin/pkg-config"
        def callback(argv):
            if argv == (pkg, "--atleast-version=4.12", "gtk4"):
                return CommandResult(argv, 0)
            if argv == (pkg, "--modversion", "gtk4"):
                return CommandResult(argv, 0, "4.22.5\n")
            return None
        runner = FakeRunner(which={"pkg-config": pkg}, callback=callback)
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            spec = PkgConfigSpec("lib.gtk4", "GTK4 >= 4.12", "gtk4", RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), minimum_version="4.12")
            result = scanner.probe_pkg_config(spec)
            self.assertEqual(result.state, CapabilityState.PASS)
            self.assertEqual(result.version, "4.22.5")

    def test_tesseract_requires_english_language_data(self) -> None:
        tess = "/usr/bin/tesseract"
        runner = FakeRunner(
            which={"tesseract": tess},
            callback=lambda argv: CommandResult(argv, 0, "List of available languages in /x (2):\nosd\neng\n") if argv == (tess, "--list-langs") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            self.assertEqual(scanner.probe_tesseract_english().state, CapabilityState.PASS)

    def test_missing_tesseract_language_is_reported(self) -> None:
        tess = "/usr/bin/tesseract"
        runner = FakeRunner(
            which={"tesseract": tess},
            callback=lambda argv: CommandResult(argv, 0, "List of available languages in /x (1):\nosd\n") if argv == (tess, "--list-langs") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            result = scanner.probe_tesseract_english()
            self.assertEqual(result.state, CapabilityState.MISSING)
            self.assertIn("eng", result.detail)

    def test_cxx26_probe_uses_syntax_only_compile(self) -> None:
        compiler = "/usr/bin/c++"
        runner = FakeRunner(
            which={"c++": compiler},
            callback=lambda argv: CommandResult(argv, 0) if argv[:3] == (compiler, "-std=c++26", "-fsyntax-only") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            result = scanner.probe_cxx26()
            self.assertEqual(result.state, CapabilityState.PASS)

    def test_opencv_ximgproc_uses_same_cmake_component_contract_as_realmheart(self) -> None:
        cmake = "/usr/bin/cmake"
        def callback(argv):
            if argv[0] == cmake and "-S" in argv and "-B" in argv:
                return CommandResult(argv, 0, "-- REALMHEART_OPENCV_VERSION=4.13.0\n")
            return None
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            scanner = CapabilityScanner(FakeRunner(which={"cmake": cmake}, callback=callback), temp_root=root / "tmp")
            result = scanner.probe_opencv_ximgproc()
            self.assertEqual(result.state, CapabilityState.PASS)
            self.assertEqual(result.version, "4.13.0")


    def test_networkmanager_probe_requires_reachable_backend(self) -> None:
        nmcli = "/usr/bin/nmcli"
        runner = FakeRunner(
            which={"nmcli": nmcli},
            callback=lambda argv: CommandResult(argv, 10, stderr="Error: NetworkManager is not running")
            if argv == (nmcli, "-t", "-f", "STATE", "general") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            result = scanner.probe_networkmanager_backend()
            self.assertEqual(result.state, CapabilityState.FAILED)
            self.assertIn("NetworkManager", result.detail)

    def test_bluetooth_without_controller_is_not_applicable(self) -> None:
        bluetoothctl = "/usr/bin/bluetoothctl"
        runner = FakeRunner(
            which={"bluetoothctl": bluetoothctl},
            callback=lambda argv: CommandResult(argv, 0, "") if argv == (bluetoothctl, "list") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            result = scanner.probe_bluetooth_backend()
            self.assertEqual(result.state, CapabilityState.NOT_APPLICABLE)
            self.assertTrue(result.satisfied)

    def test_powerprofiles_probe_requires_backend(self) -> None:
        ctl = "/usr/bin/powerprofilesctl"
        runner = FakeRunner(
            which={"powerprofilesctl": ctl},
            callback=lambda argv: CommandResult(argv, 1, stderr="power-profiles-daemon unavailable")
            if argv == (ctl, "list") else None,
        )
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(runner, temp_root=Path(temp))
            result = scanner.probe_power_profiles_backend()
            self.assertEqual(result.state, CapabilityState.FAILED)

    def test_privileged_file_tools_are_install_lifecycle(self) -> None:
        self.assertEqual(DependencyLifecycle.INSTALL.value, "install")

    def test_pam_probe_compiles_and_links_against_libpam(self) -> None:
        compiler = "/usr/bin/c++"
        def callback(argv):
            if argv[0] == compiler and "-lpam" in argv:
                return CommandResult(argv, 0)
            return None
        with tempfile.TemporaryDirectory() as temp:
            scanner = CapabilityScanner(FakeRunner(which={"c++": compiler}, callback=callback), temp_root=Path(temp))
            result = scanner.probe_pam_devel()
            self.assertEqual(result.state, CapabilityState.PASS)


if __name__ == "__main__":
    unittest.main()
