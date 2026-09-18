"""Repair execution requires explicit consent, bounded runners, and verification."""
from __future__ import annotations

import unittest

from realmheart_doctor.repair import (
    ACTION_POST_CHECKS,
    ACTION_REBUILD,
    RISK_CONFIRM,
    RISK_SAFE,
    RepairAction,
    RepairPlan,
)
from realmheart_doctor.repair_runners import RunnerOutcome, run_repair_plan


def _plan(*actions: RepairAction) -> RepairPlan:
    return RepairPlan("demo", "fixture", tuple(actions))


def _never(action) -> bool:
    raise AssertionError("consent must not be consulted for SAFE actions")


class RepairExecutionTests(unittest.TestCase):
    def test_unconsented_action_is_never_executed(self):
        def bomb(action):
            raise AssertionError("runner must not run without consent")

        report = run_repair_plan(
            _plan(RepairAction(ACTION_REBUILD, RISK_CONFIRM, "rebuild demo", targets=("demo-target",))),
            consent=lambda action: False,
            runners={ACTION_REBUILD: bomb},
        )
        self.assertEqual(report.executions[0].status, "skipped_no_consent")
        self.assertFalse(report.verified)

    def test_safe_actions_do_not_ask_for_consent(self):
        report = run_repair_plan(
            _plan(RepairAction(ACTION_POST_CHECKS, RISK_SAFE, "verify")),
            consent=_never,
            verifier=lambda: ("healthy", "checks passed"),
        )
        self.assertEqual(report.executions[0].status, "verified")
        self.assertTrue(report.verified)

    def test_failed_action_stops_further_repairs_but_still_verifies(self):
        calls = []

        def failing(action):
            calls.append(action.action_type)
            return RunnerOutcome("failed", "build failed with status 1")

        report = run_repair_plan(
            _plan(
                RepairAction(ACTION_REBUILD, RISK_CONFIRM, "rebuild demo", targets=("demo-target",)),
                RepairAction(ACTION_REBUILD, RISK_CONFIRM, "rebuild demo again", targets=("other-target",)),
                RepairAction(ACTION_POST_CHECKS, RISK_SAFE, "verify"),
            ),
            consent=lambda action: True,
            runners={ACTION_REBUILD: failing},
            verifier=lambda: ("failed", "still failing"),
        )
        self.assertEqual([item.status for item in report.executions],
                         ["failed", "skipped_after_failure", "unverified"])
        self.assertEqual(calls, [ACTION_REBUILD])
        self.assertFalse(report.verified)

    def test_repeated_action_fingerprints_are_not_replayed(self):
        action = RepairAction(ACTION_REBUILD, RISK_CONFIRM, "rebuild demo", targets=("demo-target",))
        report = run_repair_plan(
            _plan(action),
            consent=lambda _action: True,
            runners={ACTION_REBUILD: lambda _action: RunnerOutcome("succeeded", "built")},
            attempted=(action.fingerprint(),),
        )
        self.assertEqual(report.executions[0].status, "already_attempted")

    def test_missing_runner_is_reported_as_unavailable(self):
        report = run_repair_plan(
            _plan(RepairAction(ACTION_REBUILD, RISK_CONFIRM, "rebuild demo", targets=("demo-target",))),
            consent=lambda _action: True,
            runners={},
        )
        self.assertEqual(report.executions[0].status, "unavailable")
        self.assertFalse(report.verified)

    def test_verification_records_the_observed_component_status(self):
        report = run_repair_plan(
            _plan(RepairAction(ACTION_POST_CHECKS, RISK_SAFE, "verify")),
            consent=lambda _action: True,
            verifier=lambda: ("degraded", "optional capability missing"),
        )
        self.assertEqual(report.component_status, "degraded")
        self.assertFalse(report.verified)
        self.assertEqual(report.executions[0].detail, "optional capability missing")


if __name__ == "__main__":
    unittest.main()
