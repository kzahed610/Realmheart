"""Classification uses observed failures, never speculation or UNKNOWN."""
from dataclasses import replace
import unittest

from realmheart_doctor.health import HealthCheckResult, HealthStatus
from realmheart_doctor.classification import classify_failure


class DoctorClassificationTests(unittest.TestCase):
    def test_observed_version_mismatch_is_high_confidence(self):
        check = HealthCheckResult("version.demo", "demo", "version_probe", HealthStatus.FAIL, "version_mismatch")
        result = classify_failure((check,))
        self.assertEqual(result.failure_class, "DEPENDENCY_VERSION_MISMATCH")
        self.assertEqual(result.confidence, "HIGH")
        self.assertEqual(result.evidence_ids, ("version.demo",))

    def test_unknown_probe_is_never_promoted_to_known_failure(self):
        check = HealthCheckResult("version.demo", "demo", "version_probe", HealthStatus.UNKNOWN, "version_mismatch")
        result = classify_failure((check,))
        self.assertEqual(result.failure_class, "UNKNOWN")
        self.assertEqual(result.confidence, "LOW")

    def test_check_order_does_not_change_primary_classification(self):
        missing = HealthCheckResult("a", "demo", "artifact_exists", HealthStatus.FAIL, "artifact_missing")
        version = replace(missing, check_id="b", check="version_probe", reason_code="version_mismatch")
        self.assertEqual(classify_failure((missing, version)), classify_failure((version, missing)))

    def test_upstream_failure_is_classified_as_a_dependency_failure(self):
        result = classify_failure((), failed_upstream=("realmheart-core",))
        self.assertEqual(result.failure_class, "COMPONENT_DEPENDENCY_FAILURE")
        self.assertEqual(result.confidence, "HIGH")
        self.assertEqual(result.evidence_ids, ("realmheart-core",))

    def test_own_evidence_wins_over_an_upstream_failure(self):
        missing = HealthCheckResult("check.demo", "demo", "artifact_exists",
                                    HealthStatus.FAIL, "artifact_missing")
        result = classify_failure((missing,), failed_upstream=("realmheart-core",))
        self.assertEqual(result.failure_class, "COMPONENT_ARTIFACT_MISSING")
