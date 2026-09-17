"""Repair execution requires explicit per-action consent and verifies outcomes."""
import tempfile
from pathlib import Path
import unittest

from .test_doctor_state import _diagnosis
from realmheart_doctor.diagnosis import ComponentHealth
from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
from realmheart_doctor.state import record_diagnosis
from realmheart_doctor.incidents import record_component_failure
from realmheart_doctor.repair import RepairAction, execute_repair


class RepairExecutionTests(unittest.TestCase):
    def test_unconsented_action_is_never_executed(self):
        calls = []
        action = RepairAction("REINSTALL_COMPONENT", "CONFIRM", "reinstall demo")
        result = execute_repair(action, consent=lambda _action: False,
                                runner=lambda argv: (_ for _ in ()).throw(AssertionError("must not run")))
        self.assertEqual(result.status, "skipped_no_consent")
        self.assertEqual(result.verified, False)

    def test_consenting_runner_failure_is_not_reported_as_verified(self):
        action = RepairAction("REINSTALL_COMPONENT", "CONFIRM", "reinstall demo")
        result = execute_repair(action, consent=lambda _action: True,
                                runner=lambda _argv: 1)
        self.assertEqual(result.status, "failed")
        self.assertEqual(result.verified, False)


if __name__ == "__main__":
    unittest.main()
