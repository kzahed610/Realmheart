from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap  # noqa: F401
from realmheart_installer.context import XdgPaths
from realmheart_installer.environment.capabilities import (
    CapabilityResult,
    CapabilityState,
    DependencyLifecycle,
    RequirementLevel,
)
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.environment.preflight import PreflightScanner, PreflightState
from realmheart_installer.models import to_jsonable


class FakeRunner:
    def __init__(self, *, package_manager: str = "pacman", version: str = "0.56.2") -> None:
        self.package_manager = package_manager
        self.version = version
        self.hyprctl = "/usr/bin/hyprctl"
        self.systemctl = "/usr/bin/systemctl"

    def which(self, executable: str) -> str | None:
        mapping = {
            "hyprctl": self.hyprctl,
            "systemctl": self.systemctl,
        }
        if executable == self.package_manager:
            mapping[executable] = f"/usr/bin/{executable}"
        if self.package_manager == "apt-get" and executable == "apt-get":
            mapping[executable] = "/usr/bin/apt-get"
        return mapping.get(executable)

    def run(self, argv, **kwargs) -> CommandResult:
        key = tuple(str(item) for item in argv)
        if key == (self.systemctl, "--user", "show-environment"):
            return CommandResult(key, 0, "A=B\n")
        if key == (self.hyprctl, "version", "-j"):
            return CommandResult(key, 0, f'{{"version":"{self.version}","branch":"main","commit":"abc","dirty":false}}')
        if key == (self.hyprctl, "monitors", "-j"):
            return CommandResult(key, 0, '[]')
        return CommandResult(key, 1, stderr="not mocked")


def capability(capability_id: str, *, state: CapabilityState = CapabilityState.PASS, requirement: RequirementLevel = RequirementLevel.REQUIRED) -> CapabilityResult:
    return CapabilityResult(
        capability_id,
        capability_id,
        state,
        requirement,
        (DependencyLifecycle.RUNTIME,),
        "test",
    )


class PreflightTests(unittest.TestCase):
    def _paths(self, root: Path) -> XdgPaths:
        for name in ("home", "cfg", "state", "data", "cache", "run", "source"):
            (root / name).mkdir()
        (root / "source/CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\nproject(Realmheart VERSION 0.7.8 LANGUAGES C CXX)\n",
            encoding="utf-8",
        )
        shutil.copytree(_bootstrap.REPO_ROOT / "components", root / "source" / "components")
        return XdgPaths.resolve(
            env={
                "HOME": str(root / "home"),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(root / "run"),
            },
            uid=1000,
        )

    def _scan(self, root: Path, runner: FakeRunner, results: tuple[CapabilityResult, ...], *, architecture: str = "x86_64"):
        scanner = PreflightScanner(
            paths=self._paths(root),
            source_root=root / "source",
            env={
                "HOME": str(root / "home"),
                "XDG_SESSION_TYPE": "wayland",
                "WAYLAND_DISPLAY": "wayland-1",
                "HYPRLAND_INSTANCE_SIGNATURE": "sig",
            },
            runner=runner,
            os_release_text=('ID=arch\nPRETTY_NAME="Arch Linux"\n' if runner.package_manager == "pacman" else 'ID=debian\nPRETTY_NAME="Debian"\n'),
            architecture=architecture,
            kernel="6.99-test",
        )
        with patch("realmheart_installer.environment.preflight.CapabilityScanner") as scanner_cls:
            scanner_cls.return_value.scan_all.return_value = results
            return scanner.scan()

    def test_pacman_ready_environment(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = self._scan(root, FakeRunner(package_manager="pacman"), (capability("core.ok"),))
            self.assertEqual(snapshot.state, PreflightState.READY)
            self.assertTrue(snapshot.ready)
            self.assertEqual(snapshot.package_manager.kind, "pacman")
            self.assertEqual(to_jsonable(snapshot)["state"], "ready")

    def test_non_pacman_machine_can_be_ready_when_prepared(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = self._scan(root, FakeRunner(package_manager="apt-get"), (capability("core.ok"),))
            self.assertEqual(snapshot.state, PreflightState.READY_UNSUPPORTED_DISTRO)
            self.assertTrue(snapshot.ready)
            self.assertFalse(snapshot.package_manager.automatic_dependency_install)

    def test_missing_required_capability_blocks(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            results = (capability("opencv.ximgproc", state=CapabilityState.MISSING),)
            snapshot = self._scan(root, FakeRunner(package_manager="pacman"), results)
            self.assertEqual(snapshot.state, PreflightState.MISSING_DEPENDENCIES)
            self.assertIn("missing dependency capability: opencv.ximgproc", snapshot.blockers)

    def test_missing_component_dependency_is_a_dependency_gap_before_install(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            results = (
                capability("core.ok"),
                capability("runtime.tesseract", state=CapabilityState.MISSING, requirement=RequirementLevel.COMPONENT),
            )
            snapshot = self._scan(root, FakeRunner(package_manager="pacman"), results)
            self.assertEqual(snapshot.state, PreflightState.MISSING_DEPENDENCIES)
            self.assertIn("missing dependency capability: runtime.tesseract", snapshot.blockers)


    def test_corrupt_managed_receipt_is_install_state_conflict(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            paths.realmheart_state.mkdir(parents=True, exist_ok=True)
            (paths.realmheart_state / "installed-state.json").write_text("{broken", encoding="utf-8")
            scanner = PreflightScanner(
                paths=paths,
                source_root=root / "source",
                env={
                    "HOME": str(root / "home"),
                    "XDG_SESSION_TYPE": "wayland",
                    "WAYLAND_DISPLAY": "wayland-1",
                    "HYPRLAND_INSTANCE_SIGNATURE": "sig",
                },
                runner=FakeRunner(package_manager="pacman"),
                os_release_text='ID=arch\nPRETTY_NAME="Arch Linux"\n',
                architecture="x86_64",
                kernel="6.99-test",
            )
            with patch("realmheart_installer.environment.preflight.CapabilityScanner") as scanner_cls:
                scanner_cls.return_value.scan_all.return_value = (capability("core.ok"),)
                snapshot = scanner.scan()
            self.assertEqual(snapshot.state, PreflightState.INSTALL_STATE_CONFLICT)
            self.assertFalse(snapshot.ready)

    def test_newer_manifest_schema_blocks_before_install(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            manifest_file = next((root / "source/components").glob("*.toml"))
            manifest_file.write_text('schema_version = 999\nrelease_version = "0.7.8"\n', encoding="utf-8")
            scanner = PreflightScanner(
                paths=paths,
                source_root=root / "source",
                env={
                    "HOME": str(root / "home"),
                    "XDG_SESSION_TYPE": "wayland",
                    "WAYLAND_DISPLAY": "wayland-1",
                    "HYPRLAND_INSTANCE_SIGNATURE": "sig",
                },
                runner=FakeRunner(package_manager="pacman"),
                os_release_text='ID=arch\nPRETTY_NAME="Arch Linux"\n',
                architecture="x86_64",
                kernel="6.99-test",
            )
            snapshot = scanner.scan()
            self.assertEqual(snapshot.state, PreflightState.SOURCE_INVALID)
            self.assertFalse(snapshot.manifest.valid)
            self.assertTrue(any("manifest" in item for item in snapshot.blockers))

    def test_invalid_source_identity_blocks_before_install(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            (root / "source/CMakeLists.txt").write_text("project(NotRealmheart VERSION 1.0.0)\n", encoding="utf-8")
            scanner = PreflightScanner(
                paths=paths,
                source_root=root / "source",
                env={
                    "HOME": str(root / "home"),
                    "XDG_SESSION_TYPE": "wayland",
                    "WAYLAND_DISPLAY": "wayland-1",
                    "HYPRLAND_INSTANCE_SIGNATURE": "sig",
                },
                runner=FakeRunner(package_manager="pacman"),
                os_release_text='ID=arch\nPRETTY_NAME="Arch Linux"\n',
                architecture="x86_64",
                kernel="6.99-test",
            )
            with patch("realmheart_installer.environment.preflight.CapabilityScanner") as scanner_cls:
                scanner_cls.return_value.scan_all.return_value = (capability("core.ok"),)
                snapshot = scanner.scan()
            self.assertEqual(snapshot.state, PreflightState.SOURCE_INVALID)
            self.assertFalse(snapshot.ready)

    def test_old_hyprland_is_unsupported_environment(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = self._scan(root, FakeRunner(package_manager="pacman", version="0.54.3"), (capability("core.ok"),))
            self.assertEqual(snapshot.state, PreflightState.UNSUPPORTED_ENVIRONMENT)
            self.assertFalse(snapshot.ready)

    def test_newer_unknown_hyprland_requires_future_override(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = self._scan(root, FakeRunner(package_manager="pacman", version="0.58.0"), (capability("core.ok"),))
            self.assertEqual(snapshot.state, PreflightState.UNKNOWN_HYPRLAND)
            self.assertFalse(snapshot.ready)

    def test_unusual_architecture_is_reported_but_capability_driven(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = self._scan(root, FakeRunner(package_manager="pacman"), (capability("core.ok"),), architecture="aarch64")
            self.assertEqual(snapshot.state, PreflightState.READY)
            self.assertTrue(any("architecture aarch64" in warning for warning in snapshot.warnings))


if __name__ == "__main__":
    unittest.main()
