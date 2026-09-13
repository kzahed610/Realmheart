from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from . import test_planning as planning_fixture
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.components import (
    HandlerStepResult,
    default_installer_bindings,
    execute_installation_components,
    resolve_component_handler_specs,
    rollback_installation_components,
)
from realmheart_installer.components.render import render_component_report, render_component_footprints
from realmheart_installer.models import ComponentState


class RecordingBackend:
    def __init__(self, *, fail_component: str | None = None, fail_step: str | None = None) -> None:
        self.fail_component = fail_component
        self.fail_step = fail_step
        self.calls: list[tuple[str, str, tuple[str, ...]]] = []
        self.rollback_calls: list[tuple[str, tuple[str, ...]]] = []
        self.counter = 0

    def _result(self, component, step: str, ids: tuple[str, ...]) -> HandlerStepResult:
        self.calls.append((component.id, step, ids))
        if component.id == self.fail_component and step == self.fail_step:
            return HandlerStepResult(False, f"synthetic {step} failure", error_code=f"test_{step}_failed")
        self.counter += 1
        return HandlerStepResult(True, f"{step} ok", operation_ids=(f"op-{self.counter}-{component.id}-{step}",))

    def commit_artifacts(self, component, actions):
        return self._result(component, "artifacts", tuple(item.artifact_id for item in actions))

    def apply_configuration(self, component, actions):
        return self._result(component, "configuration", tuple(item.id for item in actions if item.will_mutate))

    def apply_services(self, component, actions):
        return self._result(component, "services", tuple(item.id for item in actions))

    def verify_component(self, component, checks):
        # Verification is meaningful even for capability-only components that
        # currently have no artifact health checks; the backend can perform
        # component-local capability/integration checks later.
        return self._result(component, "verify", tuple(item.id for item in checks))

    def rollback_component(self, component, operation_ids, requirements):
        self.rollback_calls.append((component.id, operation_ids))
        return HandlerStepResult(True, "rollback ok")


class Phase12ComponentHandlerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = load_manifest(_bootstrap.REPO_ROOT / "components")

    def _plan(self, root: Path):
        helper = planning_fixture.PlanningTests(methodName="test_plan_serializes_with_manifest_and_build_graph")
        helper.setUp()
        paths = helper._paths(root)
        snapshot = helper._snapshot(paths)
        return helper._build(root, snapshot, txid="RH-PHASE12-TEST")

    def test_every_canonical_component_resolves_meaningful_handler_footprint(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plan = self._plan(Path(temp))
            specs = resolve_component_handler_specs(plan, self.registry)
            self.assertEqual(len(specs), 18)
            self.assertEqual(tuple(item.component.id for item in specs), self.registry.component_order)
            self.assertTrue(all(item.component.name for item in specs))
            self.assertTrue(all(item.footprint.health_check_ids == item.component.health_check_ids for item in specs))

            core = next(item for item in specs if item.component.id == "realmheart-core")
            self.assertIn("core.binary", core.footprint.artifact_ids)
            self.assertIn("core.service", core.footprint.artifact_ids)
            self.assertIn("realmheart-shell", core.footprint.build_unit_ids)
            self.assertTrue(core.footprint.privileged_targets)

            terminal = next(item for item in specs if item.component.id == "terminal")
            self.assertTrue(terminal.service_actions_owned_by_configuration)
            self.assertIn("config.kitty.managed-block", terminal.footprint.config_action_ids)
            self.assertIn("terminal.generated-starship", terminal.footprint.artifact_ids)
            self.assertTrue(terminal.footprint.rollback_requirements)

    def test_all_components_execute_with_real_names_and_no_reserved_binding_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plan = self._plan(Path(temp))
            backend = RecordingBackend()
            report = execute_installation_components(
                plan,
                self.registry,
                backend,
                bindings=default_installer_bindings(),
                package_actions_applied=True,
            )
            self.assertEqual(len(report.results), 18)
            self.assertTrue(all(item.state is ComponentState.PASS for item in report.results))
            self.assertFalse(any(item.error_code == "binding_not_implemented" for item in report.results))
            rendered = render_component_report(report)
            self.assertIn("[01/18] Realmheart Core", rendered)
            self.assertIn("Realmheart FX", rendered)
            self.assertIn("Lockscreen Authentication", rendered)
            self.assertIn("Realmheart Terminal", rendered)

    def test_local_screenshot_failure_blocks_only_dependent_branches(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plan = self._plan(Path(temp))
            backend = RecordingBackend(fail_component="screenshot", fail_step="artifacts")
            report = execute_installation_components(plan, self.registry, backend, package_actions_applied=True)
            results = {item.component_id: item for item in report.results}
            self.assertEqual(results["screenshot"].state, ComponentState.FAILED)
            self.assertEqual(results["lens"].state, ComponentState.BLOCKED)
            self.assertEqual(results["screenshot-ocr"].state, ComponentState.BLOCKED)
            self.assertIn("screenshot", results["lens"].blocked_by)
            self.assertEqual(results["recorder"].state, ComponentState.PASS)
            self.assertEqual(results["terminal"].state, ComponentState.PASS)

    def test_terminal_configuration_owns_watcher_service_activation_once(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plan = self._plan(Path(temp))
            backend = RecordingBackend()
            execute_installation_components(plan, self.registry, backend, package_actions_applied=True)
            terminal_calls = [item for item in backend.calls if item[0] == "terminal"]
            self.assertIn("configuration", [item[1] for item in terminal_calls])
            self.assertNotIn("services", [item[1] for item in terminal_calls])
            event_calls = [item for item in backend.calls if item[0] == "event-surface"]
            self.assertIn("services", [item[1] for item in event_calls])

    def test_rollback_runs_mutated_components_in_reverse_component_order(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plan = self._plan(Path(temp))
            backend = RecordingBackend()
            report = execute_installation_components(plan, self.registry, backend, package_actions_applied=True)
            errors = rollback_installation_components(report, plan, self.registry, backend)
            self.assertEqual(errors, ())
            mutated = [item.component_id for item in report.results if item.operation_ids]
            rolled = [item[0] for item in backend.rollback_calls]
            self.assertEqual(rolled, list(reversed(mutated)))

    def test_footprint_renderer_names_meaningful_subsystems(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plan = self._plan(Path(temp))
            rendered = render_component_footprints(resolve_component_handler_specs(plan, self.registry))
            self.assertIn("Realmheart Core", rendered)
            self.assertIn("Clipboard History", rendered)
            self.assertIn("Hyprland Configuration", rendered)
            self.assertIn("Realmheart Terminal", rendered)
            self.assertNotIn("component-001", rendered)


if __name__ == "__main__":
    unittest.main()
