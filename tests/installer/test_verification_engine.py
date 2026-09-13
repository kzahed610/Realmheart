from __future__ import annotations

import os
import shutil
import stat
import tempfile
import unittest
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.configuration.terminal import KITTY_BEGIN, KITTY_END, kitty_managed_body, render_action_content
from realmheart_installer.context import XdgPaths
from realmheart_installer.environment.capabilities import CapabilityResult, CapabilityState, DependencyLifecycle, RequirementLevel
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.environment.detect import DistroInfo, HyprlandInfo, PackageManagerInfo, SessionInfo
from realmheart_installer.environment.installation import InstallOrigin, InstallationState, SourceIdentity, VersionEvidence
from realmheart_installer.environment.preflight import EnvironmentSnapshot, FilesystemCheck, ManifestSnapshot, PreflightState
from realmheart_installer.environment.support import HyprlandCompatibility, parse_version
from realmheart_installer.models import InstallMode
from realmheart_installer.native_build.models import BuildProvenance, BuildStageReport, BuildStageState, StagedArtifactResult
from realmheart_installer.planning.models import ConfigActionKind
from realmheart_installer.planning.planner import InstallationPlanner
from realmheart_installer.verification import ActivationState, ComponentHealthState, InstallHealthState, VerificationEngine, VerificationCheckState


class VerificationRunner:
    def __init__(self, *, fail_event_ping: bool = False, inactive_services: tuple[str, ...] = ()) -> None:
        self.fail_event_ping = fail_event_ping
        self.inactive_services = set(inactive_services)

    def which(self, executable):
        return "/usr/bin/" + executable

    def run(self, argv, **kwargs):
        command = tuple(str(item) for item in argv)
        if len(command) >= 4 and command[1:3] == ("--user", "is-enabled"):
            return CommandResult(command, 0, "enabled\n")
        if len(command) >= 4 and command[1:3] == ("--user", "is-active"):
            if command[3] in self.inactive_services:
                return CommandResult(command, 3, "inactive\n")
            return CommandResult(command, 0, "active\n")
        if len(command) == 2 and command[1] == "--version" and command[0].endswith("/realmheart"):
            return CommandResult(command, 0, "Realmheart 0.7.8\n")
        if len(command) == 2 and command[1] == "--help" and command[0].endswith("/realmheart-event"):
            return CommandResult(command, 0, "help\n")
        if len(command) == 2 and command[1] == "ping" and command[0].endswith("/realmheart-event"):
            if self.fail_event_ping:
                return CommandResult(command, 1, stderr="daemon protocol unavailable")
            return CommandResult(command, 0, '{"ok":true}\n')
        # Phase-11 syntax/render probes are observational and can be treated as
        # successful here; file-content/TOML checks still execute for real.
        if command and ("py_compile" in command or command[0].endswith("/fish") or command[0].endswith("/starship")):
            return CommandResult(command, 0, "ok\n")
        return CommandResult(command, 0, "ok\n")


class Phase13VerificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = load_manifest(_bootstrap.REPO_ROOT / "components")

    def _paths(self, root: Path) -> XdgPaths:
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
        return XdgPaths.resolve(env=env, uid=os.getuid())

    def _capabilities(self):
        results = []
        for spec in self.registry.capabilities_in_order():
            executable = spec.probe.args.get("executable")
            results.append(CapabilityResult(
                capability_id=spec.id,
                display_name=spec.display_name,
                state=CapabilityState.PASS,
                requirement=RequirementLevel(spec.requirement),
                lifecycle=tuple(DependencyLifecycle(value) for value in spec.lifecycle),
                detail="available",
                version="test-version" if spec.probe.args.get("version_argv") else None,
                executable=("/usr/bin/" + str(executable)) if executable else None,
                component=spec.component_id,
            ))
        return tuple(results)

    def _snapshot(self, paths: XdgPaths):
        version = parse_version("0.7.8")
        source = SourceIdentity(str(_bootstrap.REPO_ROOT), version, "0.7.8", str(_bootstrap.REPO_ROOT / "CMakeLists.txt"), "deadbeef", False)
        installation = InstallationState(
            origin=InstallOrigin.NONE,
            source=source,
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
            hyprland=HyprlandInfo("/usr/bin/hyprctl", True, "0.56.2", parse_version("0.56.2"), HyprlandCompatibility.PREFERRED, commit="abc", abi_hash="abi-test", dirty=False),
            displays=(),
            capabilities=self._capabilities(),
            filesystem=(FilesystemCheck(str(paths.config_home), 10**9, True, True, "test"),),
            installation=installation,
            manifest=ManifestSnapshot(True, self.registry.schema_version, self.registry.release_version, self.registry.digest, len(self.registry.components), len(self.registry.dependencies), len(self.registry.capabilities), len(self.registry.artifacts), len(self.registry.build_units)),
            state=PreflightState.READY,
            blockers=(), warnings=(),
        )

    def _fixture(self, root: Path):
        paths = self._paths(root)
        runner = VerificationRunner()
        snapshot = self._snapshot(paths)
        plan = InstallationPlanner(
            paths=paths,
            source_root=_bootstrap.REPO_ROOT,
            snapshot=snapshot,
            registry=self.registry,
            runner=runner,
            transaction_id="RH-VERIFY-TEST",
            prefix=root / "prefix",
            sysconf=root / "etc",
        ).build()
        self.assertTrue(plan.ready, plan.blockers)
        self._materialize_observed_install(plan, paths)
        build_report = self._build_report(plan)
        return paths, runner, plan, build_report

    def _materialize_observed_install(self, plan, paths: XdgPaths) -> None:
        # First materialize every canonical artifact so structural verification
        # has a complete installed target. Configuration actions below replace
        # placeholders with exact rendered content where required.
        for action in plan.artifact_actions:
            path = Path(action.target)
            if action.artifact_type == "directory":
                path.mkdir(parents=True, exist_ok=True)
                (path / ".verification-fixture").write_text(action.artifact_id + "\n")
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(action.artifact_id + "\n")
                if action.artifact_type == "executable":
                    path.chmod(0o755)
                else:
                    path.chmod(0o644)
        auth = Path(next(a.target for a in plan.artifact_actions if a.artifact_id == "auth.helper"))
        auth.chmod(0o4755)
        pam = Path(next(a.target for a in plan.artifact_actions if a.artifact_id == "auth.pam"))
        pam.write_bytes((_bootstrap.REPO_ROOT / "config/pam/realmheart-lockscreen").read_bytes())
        pam.chmod(0o644)

        for action in plan.config_actions:
            target = Path(action.target)
            if action.kind is ConfigActionKind.FULL_TREE_REPLACE:
                if target.exists():
                    shutil.rmtree(target)
                shutil.copytree(Path(action.source), target)
            elif action.kind is ConfigActionKind.MANAGED_BLOCK:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(f"# personal kitty line\n{KITTY_BEGIN}\n{kitty_managed_body(action)}\n{KITTY_END}\n")
            elif action.kind is ConfigActionKind.READ_ONLY:
                # The planner observed this as absent; absence therefore proves
                # that Realmheart did not mutate it.
                pass
            elif action.kind in {ConfigActionKind.OWNED_FILE, ConfigActionKind.RENDERED_FILE}:
                target.parent.mkdir(parents=True, exist_ok=True)
                if action.source is not None:
                    content, mode = render_action_content(action, source_root=_bootstrap.REPO_ROOT)
                    target.write_bytes(content)
                    target.chmod(mode)
                else:
                    # Source-less generated contracts are independently checked
                    # structurally/service-wise in Phase 13.
                    target.write_text("[Unit]\nDescription=Realmheart fixture\n[Service]\nExecStart=/bin/true\n[Install]\nWantedBy=default.target\n")
                    target.chmod(0o755 if target.name == "realmheart-lock-session" else 0o644)
            elif action.kind is ConfigActionKind.SHARED_SEED and action.will_mutate:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(action.source, target)
            elif action.kind is ConfigActionKind.GENERATED_STATE:
                target.mkdir(parents=True, exist_ok=True)

        generated = {a.artifact_id: Path(a.target) for a in plan.artifact_actions if a.artifact_type == "generated"}
        generated["terminal.generated-kitty"].write_text("foreground #ffffff\n")
        generated["terminal.generated-fish"].write_text("set -gx REALMHEART_THEME test\n")
        generated["terminal.generated-starship"].write_text('format = "$character"\n[character]\nsuccess_symbol = ">"\n')
        generated["terminal.generated-rail"].write_bytes(b"PNG-fixture")
        for path in generated.values():
            path.chmod(0o644)

    def _build_report(self, plan) -> BuildStageReport:
        provenance = BuildProvenance(
            realmheart_version=plan.target_version,
            source_revision=plan.source_revision,
            source_dirty=plan.source_dirty,
            manifest_digest=plan.manifest_digest,
            plan_digest=plan.plan_digest,
            cmake_version="cmake test",
            ninja_version="ninja test",
            cxx_compiler="c++",
            cxx_compiler_version="c++ test",
            cmake_generator="Ninja",
            cmake_build_type="Release",
            cmake_install_prefix=plan.layout.prefix,
            cmake_install_sysconfdir=plan.layout.sysconf,
            eventd_autostart="OFF",
            hyprland_version=plan.fx_plan.hyprland_version,
            hyprland_commit=plan.fx_plan.hyprland_commit,
            hyprland_abi_hash=plan.fx_plan.hyprland_abi_hash,
            fx_build_id=plan.fx_plan.build_id,
        )
        return BuildStageReport(
            schema_version=1,
            transaction_id=plan.transaction_id,
            state=BuildStageState.PASS,
            build_dir=plan.build.build_dir,
            stage_dir=plan.build.stage_dir,
            configured=True,
            required_targets_built=True,
            self_checks_passed=True,
            staged_install_completed=True,
            live_targets_unchanged=True,
            drifted_live_targets=(),
            eventd_unit_unchanged=True,
            eventd_runtime_signature_unchanged=True,
            build_units=(), artifacts=(), commands=(), provenance=provenance,
            staged_payload_bytes=1,
            accounted_uncommitted_stage_paths=(), unexpected_stage_paths=(), warnings=(), blockers=(),
        )

    def _engine(self, paths, runner, plan, build_report, *, caps=None):
        return VerificationEngine(
            plan=plan,
            registry=self.registry,
            runner=runner,
            paths=paths,
            source_root=_bootstrap.REPO_ROOT,
            build_report=build_report,
            capability_results=caps or self._capabilities(),
            verified_at=datetime(2026, 9, 12, 12, 0, tzinfo=timezone.utc),
            privileged_uid=os.getuid(),
            privileged_gid=os.getgid(),
        )

    def test_healthy_observed_install_produces_pending_activation_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            report = self._engine(paths, runner, plan, build).run()
            self.assertEqual(report.install_health, InstallHealthState.HEALTHY, report.blockers)
            self.assertEqual(report.activation.state, ActivationState.PENDING_SESSION_RESTART)
            self.assertTrue(all(item.state not in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED} for item in report.components))
            self.assertEqual(report.receipt_inputs.manifest_set_sha256, plan.manifest_digest)
            self.assertEqual(report.receipt_inputs.activation_state, "pending_session_restart")
            self.assertEqual(report.receipt_inputs.runtime_health, "unknown")

    def test_copied_but_broken_executable_fails_structural_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            core = Path(next(a.target for a in plan.artifact_actions if a.artifact_id == "core.binary"))
            core.chmod(0o644)
            report = self._engine(paths, runner, plan, build).run()
            self.assertEqual(report.install_health, InstallHealthState.FAILED)
            core_component = next(item for item in report.components if item.component_id == "realmheart-core")
            self.assertEqual(core_component.state, ComponentHealthState.FAILED)
            self.assertTrue(any(c.id == "check.core.binary.executable" and c.state is VerificationCheckState.FAILED for c in report.checks))

    def test_auth_helper_security_contract_is_core_critical(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            helper = Path(next(a.target for a in plan.artifact_actions if a.artifact_id == "auth.helper"))
            helper.chmod(0o755)
            report = self._engine(paths, runner, plan, build).run()
            auth = next(item for item in report.components if item.component_id == "lockscreen-auth")
            self.assertEqual(auth.state, ComponentHealthState.FAILED)
            self.assertEqual(report.install_health, InstallHealthState.FAILED)
            self.assertTrue(any(c.id == "verify.security.auth-helper.mode" and c.state is VerificationCheckState.FAILED for c in report.checks))

    def test_capability_only_component_gets_real_health(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            caps = list(self._capabilities())
            for index, item in enumerate(caps):
                if item.component == "night-light":
                    caps[index] = replace(item, state=CapabilityState.MISSING, detail="hyprsunset disappeared")
            report = self._engine(paths, runner, plan, build, caps=tuple(caps)).run()
            night = next(item for item in report.components if item.component_id == "night-light")
            self.assertEqual(night.state, ComponentHealthState.FAILED)
            self.assertEqual(report.install_health, InstallHealthState.DEGRADED)

    def test_receipt_hashes_immutable_release_not_mutable_user_config(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            report = self._engine(paths, runner, plan, build).run()
            identities = {item.artifact_id: item for item in report.receipt_inputs.artifacts}
            self.assertIsNotNone(identities["core.binary"].sha256)
            self.assertIsNotNone(identities["fx.plugin"].sha256)
            self.assertIsNone(identities["terminal.kitty-dropin"].sha256)
            self.assertIsNone(identities["realmheart.config"].immutable_fingerprint)

    def test_build_provenance_mismatch_fails_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            bad = replace(build, provenance=replace(build.provenance, manifest_digest="wrong"))
            report = self._engine(paths, runner, plan, bad).run()
            self.assertEqual(report.install_health, InstallHealthState.FAILED)
            self.assertTrue(any(c.id == "verify.provenance.build-report" and c.state is VerificationCheckState.FAILED for c in report.checks))



    def test_event_surface_protocol_failure_is_attributed_to_essential_component(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, _, plan, build = self._fixture(Path(temp))
            runner = VerificationRunner(fail_event_ping=True)
            report = self._engine(paths, runner, plan, build).run()
            event = next(item for item in report.components if item.component_id == "event-surface")
            self.assertEqual(event.state, ComponentHealthState.FAILED)
            self.assertEqual(report.install_health, InstallHealthState.FAILED)
            self.assertTrue(any(c.id == "verify.smoke.event-cli.ping" and c.state is VerificationCheckState.FAILED for c in report.checks))

    def test_personal_fish_config_drift_is_detected_as_terminal_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            fish = paths.config_home / "fish/config.fish"
            fish.parent.mkdir(parents=True, exist_ok=True)
            fish.write_text("# mutated during install\n")
            report = self._engine(paths, runner, plan, build).run()
            terminal = next(item for item in report.components if item.component_id == "terminal")
            self.assertEqual(terminal.state, ComponentHealthState.FAILED)
            self.assertEqual(report.install_health, InstallHealthState.DEGRADED)
            self.assertTrue(any(c.id == "verify.config.fish.personal-untouched" and c.state is VerificationCheckState.FAILED for c in report.checks))

    def test_live_immutable_artifact_must_match_validated_stage_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            core = Path(next(a.target for a in plan.artifact_actions if a.artifact_id == "core.binary"))
            import hashlib
            expected_sha = hashlib.sha256(core.read_bytes()).hexdigest()
            staged = StagedArtifactResult(
                artifact_id="core.binary",
                target_path=str(core),
                staged_path="/stage/usr/local/bin/realmheart",
                artifact_type="executable",
                required=True,
                exists=True,
                type_ok=True,
                executable_ok=True,
                mode="0o755",
                size_bytes=core.stat().st_size,
                sha256=expected_sha,
                fingerprint=None,
                reason=None,
            )
            build = replace(build, artifacts=(staged,))
            core.write_text("different live bytes\n")
            core.chmod(0o755)
            report = self._engine(paths, runner, plan, build).run()
            self.assertEqual(report.install_health, InstallHealthState.FAILED)
            self.assertTrue(any(c.id.endswith("core.binary.stage-identity") and c.state is VerificationCheckState.FAILED for c in report.checks))

    def test_receipt_uses_observed_artifact_hash_not_manifest_assumption(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            core = Path(next(a.target for a in plan.artifact_actions if a.artifact_id == "core.binary"))
            core.write_text("machine-specific observed binary bytes\n")
            core.chmod(0o755)
            report = self._engine(paths, runner, plan, build).run()
            observed = next(item for item in report.receipt_inputs.artifacts if item.artifact_id == "core.binary")
            import hashlib
            self.assertEqual(observed.sha256, hashlib.sha256(core.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
