from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.context import XdgPaths
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.environment.installation import (
    InstallOrigin,
    VersionEvidence,
    detect_installation_state,
    detect_source_identity,
)
from realmheart_installer.models import InstallMode


class FakeRunner:
    def __init__(self, *, versions: dict[str, str] | None = None) -> None:
        self.versions = versions or {}

    def which(self, executable: str) -> str | None:
        return None

    def run(self, argv, **kwargs) -> CommandResult:
        key = tuple(str(item) for item in argv)
        if len(key) == 2 and key[1] == "--version" and key[0] in self.versions:
            return CommandResult(key, 0, f"Realmheart {self.versions[key[0]]}\n")
        return CommandResult(key, 1, stderr="not mocked")


class InstallationDetectionTests(unittest.TestCase):
    def _layout(self, root: Path, *, target_version: str = "0.7.8") -> tuple[XdgPaths, Path]:
        home = root / "home"
        source = root / "source"
        for path in (home, source, root / "cfg", root / "state", root / "data", root / "cache", root / "run"):
            path.mkdir(parents=True, exist_ok=True)
        (source / "CMakeLists.txt").write_text(
            f"cmake_minimum_required(VERSION 3.25)\nproject(Realmheart VERSION {target_version} LANGUAGES C CXX)\n",
            encoding="utf-8",
        )
        paths = XdgPaths.resolve(
            env={
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(root / "run"),
            },
            uid=1000,
        )
        return paths, source

    def test_fresh_install_when_no_existing_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source = self._layout(Path(temp))
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertEqual(state.origin, InstallOrigin.NONE)
            self.assertEqual(state.mode, InstallMode.FRESH)
            self.assertEqual(state.source.version_text, "0.7.8")
            self.assertFalse(state.requires_pre_adoption_snapshot)

    def test_managed_receipt_drives_upgrade_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source = self._layout(Path(temp), target_version="0.8.0")
            paths.realmheart_state.mkdir(parents=True)
            (paths.realmheart_state / "installed-state.json").write_text(
                json.dumps({"schema_version": 2, "realmheart_version": "0.7.8", "disposition": "kept"}),
                encoding="utf-8",
            )
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertEqual(state.origin, InstallOrigin.MANAGED_INSTALLER)
            self.assertEqual(state.version_evidence, VersionEvidence.RECEIPT)
            self.assertEqual(state.mode, InstallMode.UPGRADE)

    def test_managed_receipt_drives_downgrade_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source = self._layout(Path(temp), target_version="0.7.0")
            paths.realmheart_state.mkdir(parents=True)
            (paths.realmheart_state / "installed-state.json").write_text(
                json.dumps({"schema_version": 2, "realmheart_version": "0.8.0"}),
                encoding="utf-8",
            )
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertEqual(state.mode, InstallMode.DOWNGRADE)

    def test_corrupt_managed_receipt_is_not_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source = self._layout(Path(temp))
            paths.realmheart_state.mkdir(parents=True)
            (paths.realmheart_state / "installed-state.json").write_text("{not-json", encoding="utf-8")
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertEqual(state.origin, InstallOrigin.MANAGED_INSTALLER)
            self.assertIsNone(state.mode)
            self.assertTrue(state.errors)


    def test_newer_receipt_schema_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source = self._layout(Path(temp))
            paths.realmheart_state.mkdir(parents=True)
            (paths.realmheart_state / "installed-state.json").write_text(
                json.dumps({"schema_version": 999, "realmheart_version": "0.7.8", "disposition": "kept"}),
                encoding="utf-8",
            )
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertIsNone(state.mode)
            self.assertTrue(any("schema 999" in error for error in state.errors))

    def test_non_kept_current_receipt_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source = self._layout(Path(temp))
            paths.realmheart_state.mkdir(parents=True)
            (paths.realmheart_state / "installed-state.json").write_text(
                json.dumps({"schema_version": 2, "realmheart_version": "0.7.8", "disposition": "rolled_back"}),
                encoding="utf-8",
            )
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertIsNone(state.mode)
            self.assertTrue(any("rolled-back target" in error for error in state.errors))

    def test_legacy_service_pointing_into_current_source_is_reinstall(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, source = self._layout(root)
            binary = source / "build-hybrid/realmheart"
            binary.parent.mkdir(parents=True)
            binary.write_text("placeholder", encoding="utf-8")
            service = paths.config_home / "systemd/user/realmheart.service"
            service.parent.mkdir(parents=True)
            service.write_text(
                "[Service]\nExecStart=" + str(binary) + " --shell --wallpaper-backend native\n",
                encoding="utf-8",
            )
            state = detect_installation_state(paths=paths, source_root=source, runner=FakeRunner())
            self.assertEqual(state.origin, InstallOrigin.LEGACY_SCRIPT)
            self.assertEqual(state.version_evidence, VersionEvidence.SOURCE_CHECKOUT)
            self.assertEqual(state.mode, InstallMode.REINSTALL)
            self.assertTrue(state.requires_pre_adoption_snapshot)

    def test_binary_version_beats_source_inference(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, source = self._layout(root, target_version="0.8.0")
            binary = source / "build-hybrid/realmheart"
            binary.parent.mkdir(parents=True)
            binary.write_text("placeholder", encoding="utf-8")
            service = paths.config_home / "systemd/user/realmheart.service"
            service.parent.mkdir(parents=True)
            service.write_text("[Service]\nExecStart=" + str(binary) + " --shell\n", encoding="utf-8")
            state = detect_installation_state(
                paths=paths,
                source_root=source,
                runner=FakeRunner(versions={str(binary): "0.7.8"}),
            )
            self.assertEqual(state.version_evidence, VersionEvidence.BINARY)
            self.assertEqual(state.mode, InstallMode.UPGRADE)

    def test_source_identity_rejects_missing_canonical_version(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "CMakeLists.txt").write_text("project(NotRealmheart VERSION 1.0.0)\n", encoding="utf-8")
            identity = detect_source_identity(root, FakeRunner())
            self.assertIsNone(identity.version)
            self.assertIn("project(Realmheart VERSION", identity.error or "")

    def test_source_identity_marks_untracked_checkout_content_dirty(self) -> None:
        class GitRunner(FakeRunner):
            def __init__(self) -> None:
                super().__init__()
                self.commands: list[tuple[str, ...]] = []

            def which(self, executable: str) -> str | None:
                return "/usr/bin/git" if executable == "git" else None

            def run(self, argv, **kwargs) -> CommandResult:
                command = tuple(str(item) for item in argv)
                self.commands.append(command)
                if "rev-parse" in command:
                    return CommandResult(command, 0, "abc123\n")
                if "status" in command:
                    return CommandResult(command, 0, "?? config/hypr/local-junk.conf\n")
                return CommandResult(command, 1, stderr="not mocked")

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "CMakeLists.txt").write_text(
                "project(Realmheart VERSION 0.7.8 LANGUAGES C CXX)\n",
                encoding="utf-8",
            )
            (root / ".git").mkdir()
            runner = GitRunner()
            identity = detect_source_identity(root, runner)
            self.assertTrue(identity.git_dirty)
            status = next(command for command in runner.commands if "status" in command)
            self.assertIn("--untracked-files=normal", status)
            self.assertNotIn("--untracked-files=no", status)


if __name__ == "__main__":
    unittest.main()
