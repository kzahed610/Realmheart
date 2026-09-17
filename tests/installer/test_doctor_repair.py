"""Repair planning stays separate from diagnosis and never acts without consent."""
import unittest

from realmheart_doctor.classification import classify_failure
from realmheart_doctor.health import HealthCheckResult, HealthStatus
from realmheart_doctor.repair import plan_repairs


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
