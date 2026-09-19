from __future__ import annotations

import json
import tempfile
import unittest
from unittest.mock import patch
from dataclasses import replace
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.context import XdgPaths
from realmheart_installer.environment.capabilities import (
    CapabilityResult,
    CapabilityState,
    DependencyLifecycle,
    RequirementLevel,
)
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.environment.detect import (
    DistroInfo,
    HyprlandInfo,
    PackageManagerInfo,
    SessionInfo,
)
from realmheart_installer.environment.installation import (
    InstallOrigin,
    InstallationState,
    SourceIdentity,
    VersionEvidence,
)
from realmheart_installer.environment.preflight import (
    EnvironmentSnapshot,
    FilesystemCheck,
    ManifestSnapshot,
    PreflightState,
)
from realmheart_installer.environment.support import HyprlandCompatibility, parse_version
from realmheart_installer.models import InstallMode, to_jsonable
from realmheart_installer.errors import PlanningInspectionError
from realmheart_installer.package_manager.pacman import PacmanAdapter
from realmheart_installer.planning.models import (
    BackupKind,
    ConfigActionKind,
    PackageActionKind,
    PlanState,
    ServiceActionKind,
)
from realmheart_installer.planning.planner import InstallationPlanner


class PassiveRunner:
    def which(self, executable):
        return None

    def run(self, argv, **kwargs):
        key = tuple(str(x) for x in argv)
        return CommandResult(key, 1, stderr="not mocked")


class FakePacmanRunner:
    def __init__(self, *, installed=None, repo=None, pending=()) -> None:
        self.installed = dict(installed or {})
        self.repo = dict(repo or {})
        self.pending = tuple(pending)

    def which(self, executable):
        if executable == "pacman":
            return "/usr/bin/pacman"
        if executable == "sudo":
            return "/usr/bin/sudo"
        if executable == "vercmp":
            return "/usr/bin/vercmp"
        return None

    def run(self, argv, **kwargs):
        key = tuple(str(x) for x in argv)
        if key[:3] == ("/usr/bin/pacman", "-Q", "--"):
            package = key[3]
            if package in self.installed:
                return CommandResult(key, 0, f"{package} {self.installed[package]}\n")
            return CommandResult(key, 1, stderr="not installed")
        if key[:3] == ("/usr/bin/pacman", "-Si", "--"):
            package = key[3]
            if package in self.repo:
                return CommandResult(key, 0, f"Repository : extra\nName : {package}\nVersion : {self.repo[package]}\n")
            return CommandResult(key, 1, stderr="not found")
        if key == ("/usr/bin/pacman", "-Qu"):
            return CommandResult(key, 0, "\n".join(self.pending) + ("\n" if self.pending else ""))
        if key and key[0] == "/usr/bin/vercmp":
            left, right = key[1], key[2]
            return CommandResult(key, 0, str(-1 if left < right else (1 if left > right else 0)))
        return CommandResult(key, 1, stderr="not mocked")


def cap(cid: str, component: str, *, state=CapabilityState.PASS, requirement=RequirementLevel.REQUIRED) -> CapabilityResult:
    return CapabilityResult(
        cid,
        cid,
        state,
        requirement,
        (DependencyLifecycle.RUNTIME,),
        "test",
        component=component,
    )


class PlanningTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = load_manifest(_bootstrap.REPO_ROOT / "components")

    def _paths(self, root: Path) -> XdgPaths:
        home = root / "home"
        cfg = root / "cfg"
        state = root / "state"
        data = root / "data"
        cache = root / "cache"
        run = root / "run"
        for path in (home, cfg, state, data, cache, run):
            path.mkdir(parents=True, exist_ok=True)
        return XdgPaths.resolve(env={
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(cfg),
            "XDG_STATE_HOME": str(state),
            "XDG_DATA_HOME": str(data),
            "XDG_CACHE_HOME": str(cache),
            "XDG_RUNTIME_DIR": str(run),
        }, uid=1000)

    def _installation(self, origin: InstallOrigin, mode: InstallMode, *, installed="0.7.8") -> InstallationState:
        version = parse_version("0.7.8")
        source = SourceIdentity(
            source_root=str(_bootstrap.REPO_ROOT),
            version=version,
            version_text="0.7.8",
            cmake_path=str(_bootstrap.REPO_ROOT / "CMakeLists.txt"),
            git_commit="deadbeef",
            git_dirty=False,
        )
        return InstallationState(
            origin=origin,
            source=source,
            installed_version=None if origin is InstallOrigin.NONE else parse_version(installed),
            installed_version_text=None if origin is InstallOrigin.NONE else installed,
            version_evidence=VersionEvidence.NONE if origin is InstallOrigin.NONE else VersionEvidence.BINARY,
            mode=mode,
            receipt_path=None,
            service_path=None,
            service_exec_start=None,
            binary_path=None,
            requires_pre_adoption_snapshot=origin in {InstallOrigin.LEGACY_SCRIPT, InstallOrigin.DEVELOPMENT},
            errors=(),
            warnings=("legacy adoption warning",) if origin is InstallOrigin.LEGACY_SCRIPT else (),
        )

    def _snapshot(
        self,
        paths: XdgPaths,
        *,
        origin=InstallOrigin.LEGACY_SCRIPT,
        mode=InstallMode.REINSTALL,
        capabilities=(),
        preflight_state=PreflightState.READY,
        blockers=(),
        hypr_compat=HyprlandCompatibility.PREFERRED,
    ) -> EnvironmentSnapshot:
        hypr = HyprlandInfo(
            "/usr/bin/hyprctl",
            True,
            "0.56.2" if hypr_compat is not HyprlandCompatibility.UNKNOWN else "0.58.0",
            parse_version("0.56.2" if hypr_compat is not HyprlandCompatibility.UNKNOWN else "0.58.0"),
            hypr_compat,
            branch="v0.56.2",
            commit="abc",
            abi_hash="abi-test",
            dirty=False,
        )
        return EnvironmentSnapshot(
            architecture="x86_64",
            kernel="6.test",
            distro=DistroInfo("arch", "Arch Linux", "Arch Linux", None, ()),
            package_manager=PackageManagerInfo("pacman", "/usr/bin/pacman", True),
            session=SessionInfo("wayland", "wayland-1", "sig", True, True, True),
            hyprland=hypr,
            displays=(),
            capabilities=tuple(capabilities),
            filesystem=(
                FilesystemCheck(str(paths.config_home), 10**9, True, True, "test"),
                FilesystemCheck(str(paths.state_home), 10**9, True, True, "test"),
                FilesystemCheck(str(paths.data_home), 10**9, True, True, "test"),
            ),
            installation=self._installation(origin, mode),
            manifest=ManifestSnapshot(
                True,
                self.registry.schema_version,
                self.registry.release_version,
                self.registry.digest,
                len(self.registry.components),
                len(self.registry.dependencies),
                len(self.registry.capabilities),
                len(self.registry.artifacts),
                len(self.registry.build_units),
            ),
            state=preflight_state,
            blockers=tuple(blockers),
            warnings=self._installation(origin, mode).warnings,
        )

    def _build(self, root: Path, snapshot: EnvironmentSnapshot, *, pacman_adapter=None, txid="RH-TEST-0001"):
        paths = self._paths(root) if not root.joinpath("home").exists() else XdgPaths.resolve(env={
            "HOME": str(root / "home"),
            "XDG_CONFIG_HOME": str(root / "cfg"),
            "XDG_STATE_HOME": str(root / "state"),
            "XDG_DATA_HOME": str(root / "data"),
            "XDG_CACHE_HOME": str(root / "cache"),
            "XDG_RUNTIME_DIR": str(root / "run"),
        }, uid=1000)
        return InstallationPlanner(
            paths=paths,
            source_root=_bootstrap.REPO_ROOT,
            snapshot=snapshot,
            registry=self.registry,
            runner=PassiveRunner(),
            transaction_id=txid,
            prefix=root / "prefix",
            sysconf=root / "etc",
            pacman_adapter=pacman_adapter,
        ).build()

    def test_legacy_reinstall_uses_pre_adoption_not_fake_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(paths)
            plan = self._build(root, snapshot)
            kinds = {item.kind for item in plan.backup_actions}
            self.assertIn(BackupKind.PRE_ADOPTION, kinds)
            self.assertNotIn(BackupKind.PERMANENT_BASELINE, kinds)
            self.assertTrue(plan.ready)

    def test_fresh_install_plans_permanent_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(paths, origin=InstallOrigin.NONE, mode=InstallMode.FRESH)
            plan = self._build(root, snapshot)
            self.assertEqual(plan.backup_actions[0].kind, BackupKind.PERMANENT_BASELINE)
            self.assertTrue(plan.ready)

    def test_managed_reinstall_without_baseline_is_blocked(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(paths, origin=InstallOrigin.MANAGED_INSTALLER, mode=InstallMode.REINSTALL)
            plan = self._build(root, snapshot)
            self.assertEqual(plan.state, PlanState.BLOCKED)
            self.assertTrue(any("missing its permanent" in item for item in plan.blockers))

    def test_config_contract_is_ownership_aware(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            user_cfg = paths.config_home / "realmheart/config.json"
            user_cfg.parent.mkdir(parents=True)
            user_cfg.write_text('{"mine":true}\n', encoding="utf-8")
            snapshot = self._snapshot(paths)
            plan = self._build(root, snapshot)
            by_id = {item.id: item for item in plan.config_actions}
            hypr = by_id["config.hypr.takeover"]
            self.assertEqual(hypr.kind, ConfigActionKind.FULL_TREE_REPLACE)
            self.assertTrue(any(path.endswith("/hypr/custom") for path in hypr.preserve))
            self.assertEqual(by_id["config.kitty.managed-block"].kind, ConfigActionKind.MANAGED_BLOCK)
            self.assertFalse(by_id["config.fish.personal"].will_mutate)
            self.assertEqual(by_id["config.realmheart.seed.config.json"].kind, ConfigActionKind.SHARED_SEED)
            self.assertFalse(by_id["config.realmheart.seed.config.json"].will_mutate)

    def test_cliphist_units_are_planned_as_rendered_templates(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            by_id = {item.id: item for item in plan.config_actions}
            self.assertEqual(by_id["config.artifact.clipboard.text-service"].kind, ConfigActionKind.RENDERED_FILE)
            self.assertEqual(by_id["config.artifact.clipboard.image-service"].kind, ConfigActionKind.RENDERED_FILE)
            self.assertIn("resolved executable", by_id["config.artifact.clipboard.text-service"].reason)


    def test_generated_user_artifacts_have_explicit_render_contracts(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            capabilities = (
                replace(cap("runtime.wl-paste", "clipboard-history"), executable="/opt/bin/wl-paste"),
                replace(cap("runtime.cliphist", "clipboard-history"), executable="/opt/bin/cliphist"),
                replace(cap("runtime.systemctl", "realmheart-core"), executable="/opt/bin/systemctl"),
                replace(cap("runtime.loginctl", "session"), executable="/opt/bin/loginctl"),
            )
            plan = self._build(root, self._snapshot(paths, capabilities=capabilities))
            by_id = {item.id: item for item in plan.config_actions}

            fx = by_id["config.artifact.fx.loader"]
            self.assertTrue(fx.source.endswith("config/bin/realmheart-fx-load"))

            core = by_id["config.generated.core.service"]
            self.assertEqual(core.render_strategy, "realmheart-core-user-service-v1")
            self.assertIn(("REALMHEART_BINARY", str(root / "prefix/bin/realmheart")), core.render_values)

            eventd = by_id["config.generated.event.service"]
            self.assertEqual(eventd.render_strategy, "realmheart-eventd-user-service-v1")

            lock = by_id["config.generated.auth.lock-session"]
            self.assertEqual(lock.render_strategy, "realmheart-lock-session-v1")
            self.assertIn(("SYSTEMCTL", "/opt/bin/systemctl"), lock.render_values)
            self.assertIn(("LOGINCTL", "/opt/bin/loginctl"), lock.render_values)

            clip = by_id["config.artifact.clipboard.text-service"]
            self.assertEqual(clip.render_strategy, "token-substitution-v1")
            self.assertIn(("@WL_PASTE@", "/opt/bin/wl-paste"), clip.render_values)
            self.assertIn(("@CLIPHIST@", "/opt/bin/cliphist"), clip.render_values)

            boot = by_id["config.artifact.doctor.boot-service"]
            self.assertEqual(boot.render_strategy, "token-substitution-v1")
            self.assertIn(("@REALMHEART_DOCTOR_BIN@", str(root / "prefix/bin/realmheart-doctor")),
                          boot.render_values)
            self.assertIn(("@REALMHEART_DOCTOR_STATE_DIR@", str(paths.state_home / "realmheart/doctor")),
                          boot.render_values)

    def test_terminal_plan_uses_state_home_generated_outputs_and_resolved_kitty_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            by_id = {item.id: item for item in plan.config_actions}
            kitty_dropin = by_id["config.artifact.terminal.kitty-dropin"]
            self.assertEqual(kitty_dropin.kind, ConfigActionKind.RENDERED_FILE)
            self.assertEqual(kitty_dropin.render_strategy, "terminal-kitty-dropin-v1")
            self.assertIn(("STATE_THEME", str(paths.state_home / "realmheart/theme/kitty-theme.conf")), kitty_dropin.render_values)
            kitty_block = by_id["config.kitty.managed-block"]
            self.assertEqual(kitty_block.render_strategy, "kitty-managed-include-v1")
            self.assertEqual(by_id["config.artifact.terminal.generator"].mode, "0755")
            self.assertIn(("INCLUDE_PATH", str(paths.config_home / "kitty/realmheart-theme.conf")), kitty_block.render_values)
            generated = {item.artifact_id: item.target for item in plan.artifact_actions if item.commit_class.value == "generated"}
            self.assertEqual(generated, {
                "terminal.generated-kitty": str(paths.state_home / "realmheart/theme/kitty-theme.conf"),
                "terminal.generated-fish": str(paths.state_home / "realmheart/theme/fish-theme.fish"),
                "terminal.generated-starship": str(paths.state_home / "realmheart/theme/starship.toml"),
                "terminal.generated-rail": str(paths.state_home / "realmheart/theme/rail.png"),
            })

    def test_core_shell_service_is_enabled_but_not_started_during_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            core = next(item for item in plan.service_actions if item.service == "realmheart.service")
            self.assertEqual(core.action, ServiceActionKind.ENABLE_ONLY)
            self.assertIn("defer", core.reason)

    def test_doctor_boot_service_is_enabled_but_deferred_to_the_session(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            boot = next(item for item in plan.service_actions
                        if item.service == "realmheart-doctor-boot.service")
            self.assertEqual(boot.action, ServiceActionKind.ENABLE_ONLY)
            self.assertIn("graphical session", boot.reason)

    def test_target_fingerprint_is_part_of_plan_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            hypr = paths.config_home / "hypr"
            hypr.mkdir()
            (hypr / "hyprland.conf").write_text("v1\n")
            snapshot = self._snapshot(paths)
            first = self._build(root, snapshot, txid="RH-SAME")
            second = self._build(root, snapshot, txid="RH-SAME")
            self.assertEqual(first.plan_digest, second.plan_digest)
            (hypr / "hyprland.conf").write_text("v2\n")
            third = self._build(root, snapshot, txid="RH-SAME")
            self.assertNotEqual(first.plan_digest, third.plan_digest)

    def test_unknown_hyprland_blocks_required_fx(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(
                paths,
                preflight_state=PreflightState.UNKNOWN_HYPRLAND,
                blockers=(),
                hypr_compat=HyprlandCompatibility.UNKNOWN,
            )
            plan = self._build(root, snapshot)
            self.assertEqual(plan.state, PlanState.BLOCKED)
            self.assertEqual(plan.fx_plan.compatibility.value, "unknown")

    def test_supported_hyprland_without_exact_abi_identity_blocks_required_fx(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(paths)
            snapshot = replace(snapshot, hyprland=replace(snapshot.hyprland, abi_hash=None))
            plan = self._build(root, snapshot)
            self.assertEqual(plan.state, PlanState.BLOCKED)
            self.assertEqual(plan.fx_plan.compatibility.value, "unknown")
            self.assertIn("commit/ABI identity", plan.fx_plan.reason)

    def test_known_incompatible_hyprland_blocks_required_fx_before_execution(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(
                paths,
                preflight_state=PreflightState.UNSUPPORTED_ENVIRONMENT,
                blockers=(),
                hypr_compat=HyprlandCompatibility.INCOMPATIBLE,
            )
            plan = self._build(root, snapshot)
            self.assertEqual(plan.state, PlanState.BLOCKED)
            self.assertEqual(plan.fx_plan.compatibility.value, "incompatible")
            self.assertTrue(any("required Realmheart FX is incompatible" in item for item in plan.blockers), plan.blockers)

    def test_resolvable_missing_capability_becomes_package_action_not_plan_blocker(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            missing = cap("opencv.ximgproc", "screenshot", state=CapabilityState.MISSING)
            snapshot = self._snapshot(
                paths,
                capabilities=(missing,),
                preflight_state=PreflightState.MISSING_DEPENDENCIES,
                blockers=("missing dependency capability: opencv.ximgproc",),
            )
            adapter = PacmanAdapter(FakePacmanRunner(repo={"opencv": "5.0.0-1"}))
            plan = self._build(root, snapshot, pacman_adapter=adapter)
            self.assertEqual(plan.state, PlanState.READY)
            self.assertEqual(tuple(item.package for item in plan.package_actions), ("opencv",))
            self.assertEqual(plan.package_actions[0].action, PackageActionKind.INSTALL)
            screenshot = next(item for item in plan.components if item.id == "screenshot")
            self.assertEqual(screenshot.dependency_state, "pending_package")

    def test_manual_missing_dependency_blocks_plan(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            missing = cap("hyprland.devel", "realmheart-core", state=CapabilityState.MISSING)
            snapshot = self._snapshot(
                paths,
                capabilities=(missing,),
                preflight_state=PreflightState.MISSING_DEPENDENCIES,
                blockers=("missing dependency capability: hyprland.devel",),
            )
            adapter = PacmanAdapter(FakePacmanRunner(repo={"hyprland": "0.56.2-1"}))
            plan = self._build(root, snapshot, pacman_adapter=adapter)
            self.assertEqual(plan.state, PlanState.BLOCKED)
            self.assertTrue(any("hyprland.devel" in item for item in plan.blockers))

    def test_plan_serializes_with_manifest_and_build_graph(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            payload = to_jsonable(plan)
            json.dumps(payload)
            self.assertEqual(payload["manifest_digest"], self.registry.digest)
            self.assertEqual(len(payload["components"]), 18)
            self.assertEqual(len(payload["build_units"]), 9)
            self.assertEqual(len(payload["plan_digest"]), 64)

    def test_build_plan_disables_live_autostart_and_uses_unprivileged_destdir(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            self.assertIn("-DREALMHEART_EVENTD_AUTOSTART=OFF", plan.build.configure_args)
            self.assertIn(("REALMHEART_EVENTD_AUTOSTART_DISABLE", "1"), plan.build.build_environment)
            self.assertIn(("REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL", "1"), plan.build.install_environment)
            self.assertTrue(any(key == "DESTDIR" for key, _ in plan.build.install_environment))
            self.assertIn("-DBUILD_TESTING=OFF", plan.build.configure_args)
            self.assertEqual(tuple(item.id for item in plan.build.verification), (
                "eventd-autostart-isolation", "fx-loader-contract", "lock-routing-contract", "screenshot-utility-contract"
            ))
            self.assertTrue(plan.build.side_effects_disabled)

    def test_unreadable_planning_target_becomes_structured_installer_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            snapshot = self._snapshot(paths)
            with patch("realmheart_installer.planning.planner.fingerprint_path", side_effect=PermissionError(13, "Permission denied", str(paths.config_home / "hypr/secret.glsl"))):
                with self.assertRaises(PlanningInspectionError) as raised:
                    self._build(root, snapshot)
            self.assertEqual(raised.exception.code, "RH_PLAN_INSPECTION_FAILED")
            self.assertIn("Permission denied", raised.exception.message)

    def test_activation_is_not_falsely_promoted_before_new_session_is_proven(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            self.assertTrue(plan.activation.requires_fresh_session_if_unproven)
            self.assertIn("pending_session_restart", plan.activation.expected_state)

    def test_privileged_security_actions_are_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            by_id = {item.id: item for item in plan.privileged_actions}
            self.assertEqual(by_id["privileged.auth-helper"].mode, "4755")
            self.assertEqual(by_id["privileged.auth-helper"].owner, "root")
            self.assertEqual(by_id["privileged.pam-service"].mode, "0644")

    def test_service_plan_includes_reload_and_cliphist_units(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = self._paths(root)
            plan = self._build(root, self._snapshot(paths))
            services = {item.service for item in plan.service_actions}
            self.assertIn("systemd --user", services)
            self.assertIn("realmheart-cliphist-text.service", services)
            self.assertIn("realmheart-cliphist-image.service", services)


if __name__ == "__main__":
    unittest.main()
