from __future__ import annotations

import os
import shutil
import stat
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.context import XdgPaths
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.environment.detect import DistroInfo, HyprlandInfo, PackageManagerInfo, SessionInfo
from realmheart_installer.environment.installation import InstallOrigin, InstallationState, SourceIdentity, VersionEvidence
from realmheart_installer.environment.preflight import EnvironmentSnapshot, FilesystemCheck, ManifestSnapshot, PreflightState
from realmheart_installer.environment.support import HyprlandCompatibility, parse_version
from realmheart_installer.models import InstallMode
from realmheart_installer.native_build import NativeBuildExecutor
from realmheart_installer.planning.planner import InstallationPlanner


class FakeBuildRunner:
    def __init__(self) -> None:
        self.plan = None
        self.fail_target: str | None = None
        self.omit_artifact: str | None = None
        self.mutate_eventd = False
        self.bad_fx_loader = False
        self.calls: list[tuple[tuple[str, ...], dict]] = []

    def which(self, executable):
        if executable in {"cmake", "ninja"}:
            return executable
        return None

    def run(self, argv, **kwargs):
        command = tuple(str(x) for x in argv)
        self.calls.append((command, kwargs))
        if command[:1] == ("cmake",) and "-S" in command and "-B" in command:
            build = Path(command[command.index("-B") + 1])
            build.mkdir(parents=True, exist_ok=True)
            source = Path(command[command.index("-S") + 1]).resolve()
            cache = {
                "CMAKE_HOME_DIRECTORY:INTERNAL": str(source),
                "CMAKE_GENERATOR:INTERNAL": "Ninja",
                "CMAKE_BUILD_TYPE:STRING": "Release",
                "CMAKE_INSTALL_PREFIX:PATH": self.plan.build.install_prefix,
                "CMAKE_INSTALL_BINDIR:PATH": "bin",
                "CMAKE_INSTALL_LIBDIR:PATH": "lib",
                "CMAKE_INSTALL_LIBEXECDIR:PATH": "libexec",
                "CMAKE_INSTALL_DATADIR:PATH": "share",
                "CMAKE_INSTALL_SYSCONFDIR:PATH": self.plan.layout.sysconf,
                "REALMHEART_EVENTD_AUTOSTART:BOOL": "OFF",
                "REALMHEART_BUILD_HYPRLAND_PLUGIN:BOOL": "ON",
                "REALMHEART_FX_BUILD_ID:UNINITIALIZED": self.plan.fx_plan.build_id,
                "REALMHEART_FX_HYPRLAND_COMMIT:UNINITIALIZED": self.plan.fx_plan.hyprland_commit or "",
                "REALMHEART_FX_HYPRLAND_ABI:UNINITIALIZED": self.plan.fx_plan.hyprland_abi_hash or "",
                "REALMHEART_FX_PLUGIN_PATH:UNINITIALIZED": str(Path(self.plan.layout.prefix) / "lib/realmheart/realmheart-fx.so"),
                "REALMHEART_ENABLE_NATIVE_WALLPAPER:BOOL": "ON",
                "REALMHEART_ENABLE_SCREENSHOT:BOOL": "ON",
                "BUILD_TESTING:BOOL": "OFF",
                "CMAKE_CXX_COMPILER:FILEPATH": "/fake/c++",
            }
            (build / "CMakeCache.txt").write_text("\n".join(f"{k}={v}" for k, v in cache.items()) + "\n")
            return CommandResult(command, 0, "configured\n")
        if command[:3] == ("cmake", "--build", self.plan.build.build_dir):
            target = command[command.index("--target") + 1]
            if target == self.fail_target:
                return CommandResult(command, 1, stderr=f"failed {target}")
            if target == "realmheart":
                binary = Path(self.plan.build.build_dir) / "realmheart"
                binary.write_text("fake\n")
                binary.chmod(0o755)
            if self.mutate_eventd and target == "realmheart_eventd":
                service = next(a for a in self.plan.config_actions if a.id == "config.generated.event.service")
                path = Path(service.target)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("mutated by fake build\n")
            return CommandResult(command, 0, f"built {target}\n")
        if len(command) == 2 and command[0] == str(Path(self.plan.build.build_dir) / "realmheart") and command[1] == "--version":
            return CommandResult(command, 0, "Realmheart 0.7.8\n")
        if command and command[0] in {"bash", "/usr/bin/bash"} and len(command) >= 2 and command[1].endswith("Tests.sh"):
            return CommandResult(command, 0, "contract passed\n")
        if command[:2] == ("cmake", "--install"):
            stage = Path(dict(kwargs.get("env") or {})["DESTDIR"])
            for action in self.plan.artifact_actions:
                if not action.required or action.commit_class.value not in {"privileged_commit", "staged_payload"}:
                    continue
                if action.artifact_id == self.omit_artifact:
                    continue
                target = Path(action.target)
                path = stage / target.relative_to("/")
                if action.artifact_type == "directory":
                    path.mkdir(parents=True, exist_ok=True)
                    (path / "payload.txt").write_text(action.artifact_id + "\n")
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(action.artifact_id + "\n")
                    if action.artifact_type == "executable":
                        path.chmod(0o755)
                    else:
                        path.chmod(0o644)
                    if action.artifact_id == "auth.helper":
                        path.chmod(0o4755)
            # CMake also stages a system-prefix FX helper; the live plan chooses
            # the user-scoped loader instead, so this should be reported only.
            extra = stage / Path(self.plan.layout.prefix).relative_to("/") / "bin/realmheart-fx-load"
            extra.parent.mkdir(parents=True, exist_ok=True)
            if self.bad_fx_loader:
                extra.write_text("stale configured loader\n")
            else:
                extra.write_text(
                    "configured fx loader\n"
                    f"build={self.plan.fx_plan.build_id}\n"
                    f"commit={self.plan.fx_plan.hyprland_commit or ''}\n"
                    f"abi={self.plan.fx_plan.hyprland_abi_hash or ''}\n"
                    f"plugin={Path(self.plan.layout.prefix) / 'lib/realmheart/realmheart-fx.so'}\n"
                )
            extra.chmod(0o755)
            return CommandResult(command, 0, "installed\n", stderr="staged helper ownership unchanged\n")
        if command == ("cmake", "--version"):
            return CommandResult(command, 0, "cmake version 4.4.2\n")
        if command == ("ninja", "--version"):
            return CommandResult(command, 0, "1.13.2\n")
        if command == ("/fake/c++", "--version"):
            return CommandResult(command, 0, "c++ fake 1.0\n")
        return CommandResult(command, 1, stderr="not mocked")


def make_paths(root: Path) -> XdgPaths:
    env = {
        "HOME": str(root / "home"),
        "XDG_CONFIG_HOME": str(root / "cfg"),
        "XDG_STATE_HOME": str(root / "state"),
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "XDG_RUNTIME_DIR": str(root / "run"),
    }
    for value in env.values():
        Path(value).mkdir(parents=True, exist_ok=True)
    return XdgPaths.resolve(env=env, uid=1000)


def make_source(root: Path) -> Path:
    source = root / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.25)\nproject(Realmheart VERSION 0.7.8 LANGUAGES C CXX)\n")
    shutil.copytree(_bootstrap.REPO_ROOT / "components", source / "components")
    for directory in ("assets", "styles", "effects"):
        (source / directory).mkdir()
        (source / directory / "placeholder.txt").write_text(directory + "\n")
    (source / "config/pam").mkdir(parents=True)
    (source / "config/pam/realmheart-lockscreen").write_text("auth required pam_unix.so\n")
    (source / "config/bin").mkdir(parents=True)
    (source / "config/bin/realmheart-fx-load").write_text("#!/bin/sh\n")
    return source


def make_snapshot(paths: XdgPaths, source: Path, registry) -> EnvironmentSnapshot:
    version = parse_version("0.7.8")
    installation = InstallationState(
        origin=InstallOrigin.NONE,
        source=SourceIdentity(str(source), version, "0.7.8", str(source / "CMakeLists.txt"), None, None),
        installed_version=None,
        installed_version_text=None,
        version_evidence=VersionEvidence.NONE,
        mode=InstallMode.FRESH,
        receipt_path=None,
        service_path=None,
        service_exec_start=None,
        binary_path=None,
        requires_pre_adoption_snapshot=False,
        errors=(),
        warnings=(),
    )
    return EnvironmentSnapshot(
        architecture="x86_64",
        kernel="test",
        distro=DistroInfo("arch", "Arch Linux", "Arch Linux", None, ()),
        package_manager=PackageManagerInfo("pacman", "/usr/bin/pacman", True),
        session=SessionInfo("wayland", "wayland-1", "sig", True, True, True),
        hyprland=HyprlandInfo("/usr/bin/hyprctl", True, "0.56.2", version, HyprlandCompatibility.PREFERRED, commit="abc", abi_hash="abi", dirty=False),
        displays=(), capabilities=(),
        filesystem=(FilesystemCheck(str(paths.config_home), 10**9, True, True, "test"),),
        installation=installation,
        manifest=ManifestSnapshot(True, registry.schema_version, registry.release_version, registry.digest, len(registry.components), len(registry.dependencies), len(registry.capabilities), len(registry.artifacts), len(registry.build_units)),
        state=PreflightState.READY, blockers=(), warnings=(),
    )


class NativeBuildTests(unittest.TestCase):
    def _fixture(self, root: Path):
        paths = make_paths(root)
        source = make_source(root)
        registry = load_manifest(source / "components")
        runner = FakeBuildRunner()
        snapshot = make_snapshot(paths, source, registry)
        plan = InstallationPlanner(
            paths=paths,
            source_root=source,
            snapshot=snapshot,
            registry=registry,
            runner=runner,
            transaction_id="RH-TEST-BUILD",
            prefix=root / "prefix",
            sysconf=root / "etc",
        ).build()
        runner.plan = plan
        return paths, source, registry, runner, plan

    def test_normal_user_build_stage_validates_required_payload_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            stale = Path(plan.build.build_dir) / "stale.txt"
            stale.parent.mkdir(parents=True, exist_ok=True)
            stale.write_text("stale")
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertTrue(report.ok, report.blockers)
            self.assertFalse(stale.exists())
            self.assertEqual(len(report.build_units), 9)
            self.assertTrue(all(item.ok for item in report.build_units))
            self.assertTrue(all(item.ok for item in report.artifacts))
            auth = next(item for item in report.artifacts if item.artifact_id == "auth.helper")
            self.assertEqual(auth.mode, "0o4755")
            self.assertEqual(report.provenance.eventd_autostart, "OFF")
            self.assertEqual(report.provenance.manifest_digest, plan.manifest_digest)
            self.assertEqual(report.provenance.fx_build_id, plan.fx_plan.build_id)
            self.assertEqual(report.provenance.cmake_source_dir, str(source))
            self.assertEqual(report.provenance.cmake_binary_dir, plan.build.build_dir)
            self.assertFalse(report.unexpected_stage_paths)
            self.assertTrue(any(path.endswith("bin/realmheart-fx-load") for path in report.accounted_uncommitted_stage_paths))

    def test_configure_contract_forces_install_layout_and_build_safety(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertTrue(report.ok, report.blockers)
            configure, kwargs = next(item for item in runner.calls if item[0][0] == "cmake" and "-S" in item[0])
            self.assertIn("-DREALMHEART_EVENTD_AUTOSTART=OFF", configure)
            self.assertIn("-DREALMHEART_BUILD_HYPRLAND_PLUGIN=ON", configure)
            self.assertIn("-DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON", configure)
            self.assertIn("-DREALMHEART_ENABLE_SCREENSHOT=ON", configure)
            self.assertIn("-DBUILD_TESTING=OFF", configure)
            self.assertIn(f"-DCMAKE_INSTALL_SYSCONFDIR={plan.layout.sysconf}", configure)
            self.assertEqual(kwargs["env"]["REALMHEART_EVENTD_AUTOSTART_DISABLE"], "1")
            install, install_kwargs = next(item for item in runner.calls if item[0][:2] == ("cmake", "--install"))
            self.assertEqual(install_kwargs["env"]["REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL"], "1")
            self.assertEqual(install_kwargs["env"]["DESTDIR"], plan.build.stage_dir)
            eventd_check, eventd_kwargs = next(item for item in runner.calls if item[0][1].endswith("EventDaemonAutostartTests.sh"))
            self.assertEqual(eventd_kwargs["env"]["REALMHEART_EVENTD_AUTOSTART_DISABLE"], "0")

    def test_symlink_inside_required_staged_directory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            original_run = runner.run

            def run_with_symlink(argv, **kwargs):
                result = original_run(argv, **kwargs)
                command = tuple(str(item) for item in argv)
                if command[:2] == ("cmake", "--install") and result.ok:
                    staged_assets = Path(plan.build.stage_dir) / Path(plan.layout.prefix).relative_to("/") / "share/realmheart/assets"
                    (staged_assets / "escape").symlink_to("/etc/passwd")
                return result

            runner.run = run_with_symlink
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertFalse(report.ok)
            self.assertTrue(any("contains symlink" in item for item in report.blockers))

    def test_missing_required_fx_in_stage_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            runner.omit_artifact = "fx.plugin"
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertFalse(report.ok)
            self.assertTrue(any("FX plugin" in item or "fx.plugin" in item for item in report.blockers))

    def test_staged_fx_loader_must_be_bound_to_approved_build_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            runner.bad_fx_loader = True
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertFalse(report.ok)
            self.assertTrue(any("FX loader is not bound" in blocker for blocker in report.blockers), report.blockers)

    def test_required_fx_build_target_failure_is_fatal_before_install(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            runner.fail_target = "realmheart_fx"
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertFalse(report.ok)
            self.assertFalse(report.staged_install_completed)
            self.assertTrue(any("FX" in item for item in report.blockers))

    def test_missing_source_prerequisite_stops_before_configure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            shutil.rmtree(source / "assets")
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertFalse(report.ok)
            self.assertTrue(any("assets" in item for item in report.blockers))
            self.assertFalse(any("-S" in call[0] for call in runner.calls))

    def test_eventd_service_file_mutation_during_build_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, source, registry, runner, plan = self._fixture(Path(temp))
            service = next(a for a in plan.config_actions if a.id == "config.generated.event.service")
            service_path = Path(service.target)
            service_path.parent.mkdir(parents=True, exist_ok=True)
            service_path.write_text("before\n")
            # Re-plan so the precondition/fingerprint matches the real pre-build service.
            snapshot = make_snapshot(paths, source, registry)
            plan = InstallationPlanner(paths=paths, source_root=source, snapshot=snapshot, registry=registry, runner=runner, transaction_id="RH-TEST-BUILD2", prefix=Path(plan.layout.prefix), sysconf=Path(plan.layout.sysconf)).build()
            runner.plan = plan
            runner.mutate_eventd = True
            report = NativeBuildExecutor(plan=plan, source_root=source, registry=registry, runner=runner, installer_cache=paths.installer_cache).run()
            self.assertFalse(report.ok)
            self.assertFalse(report.eventd_unit_unchanged)
            self.assertTrue(any("eventd user service changed" in item for item in report.blockers))


if __name__ == "__main__":
    unittest.main()
