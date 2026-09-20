"""Repair planning stays separate from diagnosis and never acts without consent."""
import unittest
from pathlib import Path

from realmheart_doctor.classification import FailureClassification, classify_failure
from realmheart_doctor.health import HealthCheckResult, HealthStatus
from realmheart_maintenance.manifest import load_manifest
from realmheart_doctor.repair import (
    ACTION_INSTALL_PACKAGE,
    ACTION_POST_CHECKS,
    ACTION_REBUILD,
    ACTION_RESTART_SERVICE,
    RISK_CONFIRM,
    RISK_PRIVILEGED,
    RISK_SAFE,
    RepairContext,
    plan_incident_repair,
    plan_repairs,
)


def _checks(*items: tuple[str, str, HealthStatus, str]):
    return tuple(HealthCheckResult(cid, "demo", check, status, reason)
                 for cid, check, status, reason in items)


class RepairPlanningTests(unittest.TestCase):
    def test_missing_required_artifact_plans_confirmed_reinstall(self):
        classification = classify_failure(_checks(
            ("check.demo", "artifact_exists", HealthStatus.FAIL, "artifact_missing")))
        plan = plan_repairs("demo", classification)
        self.assertIsNotNone(plan)
        self.assertEqual(plan.component_id, "demo")
        self.assertEqual(plan.actions[0].action_type, "REINSTALL_COMPONENT")
        self.assertEqual(plan.actions[0].risk, "CONFIRM")

    def test_unobserved_failure_never_produces_a_repair_plan(self):
        classification = classify_failure(_checks(
            ("check.demo", "artifact_exists", HealthStatus.UNKNOWN, "artifact_missing")))
        self.assertIsNone(plan_repairs("demo", classification))

    def test_missing_capability_evidence_classifies_as_dependency_missing(self):
        classification = classify_failure((), missing_capabilities=("runtime.grim",))
        self.assertEqual(classification.failure_class, "DEPENDENCY_MISSING")
        self.assertEqual(classification.evidence_ids, ("runtime.grim",))
        self.assertEqual(classify_failure(()).failure_class, "UNKNOWN")


class RepairContextPlanningTests(unittest.TestCase):
    def test_missing_dependency_plans_a_privileged_install_then_verification(self):
        classification = FailureClassification("DEPENDENCY_MISSING", "HIGH", ("runtime.grim",))
        context = RepairContext("lens", ("install_missing_package",), packages=("grim",))
        plan = plan_repairs("lens", classification, context=context)
        self.assertEqual([item.action_type for item in plan.actions],
                         [ACTION_INSTALL_PACKAGE, ACTION_POST_CHECKS])
        self.assertEqual(plan.actions[0].risk, RISK_PRIVILEGED)
        self.assertEqual(plan.actions[0].packages, ("grim",))
        self.assertEqual(plan.actions[1].risk, RISK_SAFE)

    def test_artifact_failure_with_rebuild_strategy_plans_a_rebuild(self):
        classification = FailureClassification("COMPONENT_ARTIFACT_MISSING", "HIGH", ("check.demo",))
        context = RepairContext("screenshot", ("rebuild_component",), build_targets=("realmheart_screenshot",))
        plan = plan_repairs("screenshot", classification, context=context)
        self.assertEqual([item.action_type for item in plan.actions], [ACTION_REBUILD, ACTION_POST_CHECKS])
        self.assertEqual(plan.actions[0].risk, RISK_CONFIRM)
        self.assertEqual(plan.actions[0].targets, ("realmheart_screenshot",))

    def test_service_failure_plans_a_user_service_restart(self):
        classification = FailureClassification("OBSERVED_FAILURE", "MEDIUM", ("check.event.socket.reachable",))
        context = RepairContext("event-surface", ("restart_service",),
                                service_units=("realmheart-eventd.service",))
        plan = plan_repairs("event-surface", classification, context=context)
        self.assertEqual([item.action_type for item in plan.actions],
                         [ACTION_RESTART_SERVICE, ACTION_POST_CHECKS])
        self.assertEqual(plan.actions[0].targets, ("realmheart-eventd.service",))

    def test_no_executable_strategy_means_no_plan(self):
        classification = FailureClassification("COMPONENT_ARTIFACT_MISSING", "HIGH", ("check.demo",))
        context = RepairContext("hypr-integration", ())
        self.assertIsNone(plan_repairs("hypr-integration", classification, context=context))


    def test_installer_bound_artifact_failure_does_not_offer_unexecutable_rebuild(self):
        classification = FailureClassification("COMPONENT_ARTIFACT_MISSING", "HIGH", ("check.demo",))
        context = RepairContext(
            "lockscreen-auth", ("rebuild_component",),
            build_targets=("realmheart_auth",), installer_bound=True,
        )
        self.assertIsNone(plan_repairs("lockscreen-auth", classification, context=context))

    def test_saved_incident_plans_contextual_repair_without_reprobing(self):
        root = Path(__file__).resolve().parents[2]
        registry = load_manifest(root / "components")
        incident = {
            "resolution_state": "unresolved",
            "component_id": "screenshot",
            "failure_class": "COMPONENT_ARTIFACT_MISSING",
            "confidence": "HIGH",
            "checks": [{"check_id": "check.screenshot.binary.exists"}],
            "capabilities": [],
        }
        plan = plan_incident_repair(registry, incident)
        self.assertIsNotNone(plan)
        self.assertEqual(plan.component_id, "screenshot")
        self.assertEqual(
            [item.action_type for item in plan.actions],
            [ACTION_REBUILD, ACTION_POST_CHECKS],
        )

    def test_action_fingerprints_are_stable_and_content_sensitive(self):
        first = RepairContext("lens", ("install_missing_package",), packages=("grim",))
        second = RepairContext("lens", ("install_missing_package",), packages=("slurp",))
        classification = FailureClassification("DEPENDENCY_MISSING", "HIGH", ("runtime.grim",))
        left = plan_repairs("lens", classification, context=first).actions[0]
        right = plan_repairs("lens", classification, context=second).actions[0]
        self.assertNotEqual(left.fingerprint(), right.fingerprint())
        self.assertEqual(left.fingerprint(),
                         plan_repairs("lens", classification, context=first).actions[0].fingerprint())


if __name__ == "__main__":
    unittest.main()
