from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from . import _bootstrap
from . import test_verification_engine as phase13
from realmheart_installer.configuration.terminal import render_action_content
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.verification import ActivationState, ComponentHealthState, InstallHealthState, VerificationEngine, VerificationCheckState


class FxRuntimeRunner(phase13.VerificationRunner):
    def __init__(self, *, build_id: str, commit: str, abi: str, plugin_build_id: str | None = None, runtime_commit: str | None = None, runtime_abi: str | None = None, plugin_loaded: bool = True) -> None:
        super().__init__()
        self.build_id = build_id
        self.commit = commit
        self.abi = abi
        self.plugin_build_id = plugin_build_id if plugin_build_id is not None else build_id
        self.runtime_commit = runtime_commit if runtime_commit is not None else commit
        self.runtime_abi = runtime_abi if runtime_abi is not None else abi
        self.plugin_loaded = plugin_loaded

    def run(self, argv, **kwargs):
        command = tuple(str(item) for item in argv)
        if len(command) == 3 and command[0].endswith("/hyprctl") and command[1:] == ("version", "-j"):
            return CommandResult(command, 0, json.dumps({"commit": self.runtime_commit, "abiHash": self.runtime_abi, "dirty": False}) + "\n")
        if len(command) == 3 and command[0].endswith("/hyprctl") and command[1:] == ("plugin", "list"):
            return CommandResult(command, 0, "Plugin: Realmheart FX\n" if self.plugin_loaded else "no plugins loaded\n")
        if len(command) == 3 and command[0].endswith("/hyprctl") and command[1:] == ("realmheart-fx", "identity"):
            if not self.plugin_loaded:
                return CommandResult(command, 1, stderr="unknown command")
            return CommandResult(
                command,
                0,
                f"build_id={self.plugin_build_id}\n"
                "realmheart_version=0.7.8\n"
                f"hyprland_commit={self.commit}\n"
                f"hyprland_abi={self.abi}\n",
            )
        return super().run(argv, **kwargs)


class Phase14FxContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.base = phase13.Phase13VerificationTests(methodName="test_healthy_observed_install_produces_pending_activation_receipt")
        self.base.setUp()

    def _fixture(self, root: Path):
        return self.base._fixture(root)

    def _engine(self, paths, runner, plan, build):
        return VerificationEngine(
            plan=plan,
            registry=self.base.registry,
            runner=runner,
            paths=paths,
            source_root=_bootstrap.REPO_ROOT,
            build_report=build,
            capability_results=self.base._capabilities(),
            privileged_uid=os.getuid(),
            privileged_gid=os.getgid(),
        )

    def test_plan_binds_loader_and_cmake_to_exact_fx_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, runner, plan, build = self._fixture(Path(temp))
            self.assertEqual(plan.schema_version, 2)
            self.assertEqual(len(plan.fx_plan.build_id), 64)
            self.assertEqual(plan.fx_plan.rebuild_on_dependency_change, ("dep.hyprland.devel",))
            loader = next(action for action in plan.config_actions if action.id == "config.artifact.fx.loader")
            self.assertEqual(loader.render_strategy, "realmheart-fx-loader-v1")
            content, _ = render_action_content(loader, source_root=_bootstrap.REPO_ROOT)
            text = content.decode()
            self.assertIn(plan.fx_plan.build_id, text)
            self.assertIn(plan.fx_plan.hyprland_commit, text)
            self.assertIn(plan.fx_plan.hyprland_abi_hash, text)
            self.assertIn(str(Path(plan.layout.prefix) / "lib/realmheart/realmheart-fx.so"), text)
            self.assertNotIn("@REALMHEART_FX_", text)
            configure = "\n".join(plan.build.configure_args)
            self.assertIn(f"REALMHEART_FX_BUILD_ID={plan.fx_plan.build_id}", configure)
            self.assertIn(f"REALMHEART_FX_HYPRLAND_COMMIT={plan.fx_plan.hyprland_commit}", configure)
            self.assertIn(f"REALMHEART_FX_HYPRLAND_ABI={plan.fx_plan.hyprland_abi_hash}", configure)

    def test_exact_running_fx_build_promotes_activation_active(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, _, plan, build = self._fixture(Path(temp))
            runner = FxRuntimeRunner(build_id=plan.fx_plan.build_id, commit=plan.fx_plan.hyprland_commit, abi=plan.fx_plan.hyprland_abi_hash)
            report = self._engine(paths, runner, plan, build).run()
            self.assertEqual(report.install_health, InstallHealthState.HEALTHY, report.blockers)
            self.assertEqual(report.activation.state, ActivationState.ACTIVE)
            self.assertTrue(report.activation.fx_runtime.matches_validated_build)
            self.assertTrue(any(check.id == "verify.fx.runtime-build-active" and check.state is VerificationCheckState.PASS for check in report.checks))

    def test_stale_loaded_fx_is_install_healthy_but_pending_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, _, plan, build = self._fixture(Path(temp))
            runner = FxRuntimeRunner(build_id=plan.fx_plan.build_id, plugin_build_id="old-build", commit=plan.fx_plan.hyprland_commit, abi=plan.fx_plan.hyprland_abi_hash)
            report = self._engine(paths, runner, plan, build).run()
            self.assertEqual(report.install_health, InstallHealthState.HEALTHY, report.blockers)
            self.assertEqual(report.activation.state, ActivationState.PENDING_SESSION_RESTART)
            self.assertFalse(report.activation.fx_runtime.matches_validated_build)
            self.assertTrue(any(check.id == "verify.fx.runtime-build-active" and check.state is VerificationCheckState.PENDING for check in report.checks))

    def test_runtime_hyprland_abi_drift_is_core_critical_fx_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, _, plan, build = self._fixture(Path(temp))
            runner = FxRuntimeRunner(
                build_id=plan.fx_plan.build_id,
                commit=plan.fx_plan.hyprland_commit,
                abi=plan.fx_plan.hyprland_abi_hash,
                runtime_abi="different-runtime-abi",
            )
            report = self._engine(paths, runner, plan, build).run()
            self.assertEqual(report.install_health, InstallHealthState.FAILED)
            self.assertTrue(any(check.id == "verify.fx.runtime-hyprland-identity" and check.state is VerificationCheckState.FAILED for check in report.checks))
            core = next(item for item in report.components if item.component_id == "realmheart-core")
            self.assertEqual(core.state, ComponentHealthState.FAILED)

    def test_missing_required_fx_prevents_core_from_reporting_healthy(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, _, plan, build = self._fixture(Path(temp))
            plugin = Path(next(action.target for action in plan.artifact_actions if action.artifact_id == "fx.plugin"))
            plugin.unlink()
            runner = FxRuntimeRunner(build_id=plan.fx_plan.build_id, commit=plan.fx_plan.hyprland_commit, abi=plan.fx_plan.hyprland_abi_hash, plugin_loaded=False)
            report = self._engine(paths, runner, plan, build).run()
            core = next(item for item in report.components if item.component_id == "realmheart-core")
            fx = next(item for item in report.components if item.component_id == "realmheart-fx")
            self.assertEqual(fx.state, ComponentHealthState.FAILED)
            self.assertEqual(core.state, ComponentHealthState.FAILED)
            self.assertTrue(any(check.id == "verify.core.required-fx" and check.state is VerificationCheckState.FAILED for check in report.checks))

    def test_receipt_contains_future_doctor_fx_rebuild_trigger(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths, _, plan, build = self._fixture(Path(temp))
            runner = FxRuntimeRunner(build_id=plan.fx_plan.build_id, commit=plan.fx_plan.hyprland_commit, abi=plan.fx_plan.hyprland_abi_hash)
            report = self._engine(paths, runner, plan, build).run()
            fx = report.receipt_inputs.fx
            self.assertEqual(fx.build_id, plan.fx_plan.build_id)
            self.assertEqual(fx.hyprland_commit, plan.fx_plan.hyprland_commit)
            self.assertEqual(fx.hyprland_abi_hash, plan.fx_plan.hyprland_abi_hash)
            self.assertEqual(len(fx.rebuild_triggers), 1)
            trigger = fx.rebuild_triggers[0]
            self.assertEqual(trigger.capability_id, "dep.hyprland.devel")
            self.assertEqual(trigger.build_unit_id, "realmheart-fx")
            self.assertEqual(trigger.trigger, "version_commit_or_abi_change")

    def test_public_loader_rejects_stale_preloaded_plugin_identity(self) -> None:
        result = subprocess.run(
            ["bash", str(_bootstrap.REPO_ROOT / "tests/RealmheartFxLoaderTests.sh"), str(_bootstrap.REPO_ROOT)],
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("confirmation contracts passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
