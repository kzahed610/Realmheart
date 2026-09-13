from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap
from realmheart_installer.validation import phase19


class Phase19ValidationTests(unittest.TestCase):
    def test_matrix_exactly_covers_phase19_real_system_scenarios(self) -> None:
        expected = {
            "clean-user",
            "custom-hypr",
            "custom-kitty",
            "custom-fish",
            "non-default-xdg",
            "conflicting-owned-files",
            "upgrade",
            "reinstall",
            "downgrade",
            "missing-soft-dependency",
            "multi-monitor",
            "broken-component",
            "controlled-interrupt",
        }
        ids = {item.scenario_id for item in phase19.PHASE19_SCENARIOS}
        self.assertEqual(ids, expected)
        self.assertEqual(len(ids), len(phase19.PHASE19_SCENARIOS))
        self.assertTrue(all(item.destructive for item in phase19.PHASE19_SCENARIOS))
        self.assertTrue(all(item.setup and item.acceptance and item.fixture_tests for item in phase19.PHASE19_SCENARIOS))

    def test_every_fixture_target_resolves_to_a_real_test(self) -> None:
        loader = unittest.TestLoader()
        for scenario in phase19.PHASE19_SCENARIOS:
            for target in scenario.fixture_tests:
                suite = loader.loadTestsFromName(target)
                errors = []
                for test in suite:
                    # FailedTest is an internal unittest placeholder produced by
                    # import/attribute resolution failure.
                    if test.__class__.__name__ == "_FailedTest":
                        errors.append(str(test))
                self.assertFalse(errors, f"{scenario.scenario_id}: unresolved target {target}: {errors}")
                self.assertGreater(suite.countTestCases(), 0, target)

    def test_report_roundtrip_is_private_and_scenario_complete(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "phase19.json"
            report = phase19.create_report(_bootstrap.REPO_ROOT)
            phase19.save_report(report, path)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            loaded = phase19.load_report(path)
            self.assertEqual(set(loaded.scenarios), {item.scenario_id for item in phase19.PHASE19_SCENARIOS})
            self.assertTrue(all(item.status is phase19.ValidationStatus.PENDING for item in loaded.scenarios.values()))

    def test_report_rejects_missing_or_unknown_scenario_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "phase19.json"
            report = phase19.create_report(_bootstrap.REPO_ROOT)
            phase19.save_report(report, path)
            payload = json.loads(path.read_text())
            payload["scenarios"].pop("clean-user")
            path.write_text(json.dumps(payload))
            with self.assertRaisesRegex(ValueError, "scenario set"):
                phase19.load_report(path)

    def test_record_result_hashes_and_privately_copies_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "install.log"
            source.write_bytes(b"scenario evidence\n")
            evidence_dir = root / "evidence"
            report = phase19.create_report(_bootstrap.REPO_ROOT)
            result = phase19.record_result(
                report,
                scenario_id="clean-user",
                status=phase19.ValidationStatus.PASS,
                note="fresh disposable user passed",
                evidence_paths=(source,),
                evidence_dir=evidence_dir,
            )
            self.assertEqual(result.status, phase19.ValidationStatus.PASS)
            self.assertEqual(result.evidence_files[0]["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            stored = evidence_dir / str(result.evidence_files[0]["stored_as"])
            self.assertEqual(stored.read_bytes(), source.read_bytes())
            self.assertEqual(stored.stat().st_mode & 0o777, 0o600)

    def test_record_result_rejects_symlink_evidence(self) -> None:
        if not hasattr(os, "symlink"):
            self.skipTest("symlink unsupported")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "real.log"; target.write_text("x")
            link = root / "link.log"; link.symlink_to(target)
            report = phase19.create_report(_bootstrap.REPO_ROOT)
            with self.assertRaisesRegex(ValueError, "regular non-symlink"):
                phase19.record_result(
                    report,
                    scenario_id="clean-user",
                    status=phase19.ValidationStatus.PASS,
                    evidence_paths=(link,),
                )

    def test_summary_requires_fixture_host_and_all_live_scenarios(self) -> None:
        report = phase19.create_report(_bootstrap.REPO_ROOT)
        for item in report.scenarios.values():
            item.fixture_status = phase19.ValidationStatus.PASS
            item.status = phase19.ValidationStatus.PASS
        report.host_audit = {"checks": {
            "manifest": {"status": "pass"},
            "install-dry-run": {"status": "pass"},
            "recovery-list": {"status": "pass"},
        }}
        summary = phase19.summarize_report(report)
        self.assertTrue(summary["phase19_complete"])
        report.scenarios["downgrade"].status = phase19.ValidationStatus.BLOCKED
        self.assertFalse(phase19.summarize_report(report)["phase19_complete"])

    def test_host_audit_stores_raw_output_only_in_private_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            report = phase19.create_report(_bootstrap.REPO_ROOT)
            outputs = iter([
                (0, b"manifest PASS /private/path\n", 0.1),
                (0, b"dry run READY /home/private\n", 0.2),
                (0, b"no recovery\n", 0.3),
            ])
            with patch.object(phase19, "_run", side_effect=lambda *a, **k: next(outputs)):
                self.assertTrue(phase19.run_host_audit(
                    report,
                    source_root=_bootstrap.REPO_ROOT,
                    evidence_dir=root,
                ))
            serialized = json.dumps(phase19._report_to_dict(report))
            self.assertNotIn("/private/path", serialized)
            self.assertNotIn("/home/private", serialized)
            for item in report.host_audit["checks"].values():
                evidence = root / "host-audit" / item["evidence"]["file"]
                self.assertTrue(evidence.is_file())
                self.assertEqual(evidence.stat().st_mode & 0o777, 0o600)

    def test_fixture_matrix_records_scenario_level_results_without_marking_live_pass(self) -> None:
        report = phase19.create_report(_bootstrap.REPO_ROOT)
        with patch.object(phase19, "_run", return_value=(0, b"OK\n", 0.01)):
            results = phase19.run_fixture_matrix(report, source_root=_bootstrap.REPO_ROOT)
        self.assertEqual(len(results), len(phase19.PHASE19_SCENARIOS))
        self.assertTrue(all(status is phase19.ValidationStatus.PASS for _, status, _ in results))
        self.assertTrue(all(item.fixture_status is phase19.ValidationStatus.PASS for item in report.scenarios.values()))
        self.assertTrue(all(item.status is phase19.ValidationStatus.PENDING for item in report.scenarios.values()))


if __name__ == "__main__":
    unittest.main()
