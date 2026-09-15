from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance import (
    ArtifactObservation,
    CurrentHealthSnapshot,
    DriftKind,
    ForensicContractError,
    ForensicReport,
    ReadinessState,
    analyze_forensics,
    load_health_snapshot,
    load_installed_receipt,
    load_manifest,
    serialize_health_snapshot,
    select_health_checks,
)
from realmheart_installer.finalization import build_installed_state_receipt


TOOL = _bootstrap.REPO_ROOT / "tools/validate-realmheart-doctor-contract.py"


class Phase20ForensicContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = load_manifest(_bootstrap.REPO_ROOT / "components")

    def _generated_receipt(self, root: Path) -> tuple[Path, dict[str, object]]:
        # Produce the input using the real installer verification + receipt
        # assembly path.  The consumer under test remains tool-independent.
        from tests.installer.test_verification_engine import Phase13VerificationTests

        helper = Phase13VerificationTests()
        helper.setUp()
        paths, runner, plan, build = helper._fixture(root / "fixture")
        fixture_root = root / "fixture"
        environment = {
            "HOME": str(fixture_root / "home"),
            "XDG_CONFIG_HOME": str(fixture_root / "cfg"),
            "XDG_STATE_HOME": str(fixture_root / "state"),
            "PREFIX": str(fixture_root / "prefix"),
            "LIBEXEC": str(fixture_root / "prefix" / "libexec"),
            "SYSCONF": str(fixture_root / "etc"),
        }
        previous_environment = {key: os.environ.get(key) for key in environment}
        os.environ.update(environment)

        def restore_environment() -> None:
            for key, value in previous_environment.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

        self.addCleanup(restore_environment)
        report = helper._engine(paths, runner, plan, build).run()
        payload = build_installed_state_receipt(plan, report)
        path = root / "installed-state.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return path, payload

    def _snapshot_from_receipt(self, root: Path, receipt: dict[str, object]) -> tuple[Path, dict[str, object]]:
        capabilities = {
            capid: {
                "state": item["state"],
                "version": item.get("version"),
                "detail": "synthetic current observation",
            }
            for capid, item in receipt["dependencies"].items()
        }
        artifacts = {
            aid: {
                "path": item["path"],
                "exists": True,
                "sha256": item.get("sha256"),
                "immutable_fingerprint": item.get("immutable_fingerprint"),
                "mode": item.get("mode"),
                "filesystem_type": "directory" if item["type"] == "directory" else "file",
            }
            for aid, item in receipt["artifacts"].items()
        }
        payload: dict[str, object] = {
            "schema_version": 2,
            "captured_at": "2026-09-13T12:00:00+00:00",
            "activation_state": receipt["activation_state"],
            "runtime_health": receipt["runtime_health"],
            "capabilities": capabilities,
            "artifacts": artifacts,
        }
        path = root / "current-health.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return path, payload

    def _path_binding_case(
        self,
        root: Path,
        *,
        receipt_path_override: str | None = None,
        snapshot_path_override: str | None = None,
        omit_snapshot_path: bool = False,
        unresolved_manifest_path: bool = False,
        optional: bool = False,
        legacy_snapshot_schema: bool = False,
        omit_receipt_artifact: bool = False,
        omit_snapshot_artifact: bool = False,
    ) -> ForensicReport:
        receipt_path, receipt_payload = self._generated_receipt(root)
        snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
        receipt_artifacts = receipt_payload["artifacts"]
        snapshot_artifacts = snapshot_payload["artifacts"]
        if not isinstance(receipt_artifacts, dict) or not isinstance(snapshot_artifacts, dict):
            self.fail("synthetic artifact records must be objects")

        for aid, item in receipt_artifacts.items():
            if not isinstance(item, dict) or not isinstance(snapshot_artifacts.get(aid), dict):
                self.fail(f"synthetic artifact {aid} records must be objects")
            snapshot_item = snapshot_artifacts[aid]
            if not isinstance(snapshot_item, dict):
                self.fail(f"synthetic artifact {aid} snapshot must be an object")
            snapshot_item["path"] = item["path"]
        canonical_paths = {
            aid: str(item["path"])
            for aid, item in receipt_artifacts.items()
            if isinstance(item, dict)
        }

        target_id = "core.binary"
        target_receipt = receipt_artifacts[target_id]
        target_snapshot = snapshot_artifacts[target_id]
        if not isinstance(target_receipt, dict) or not isinstance(target_snapshot, dict):
            self.fail("synthetic core.binary records must be objects")
        if receipt_path_override is not None:
            target_receipt["path"] = receipt_path_override
        if snapshot_path_override is not None:
            target_snapshot["path"] = snapshot_path_override
        if omit_snapshot_path:
            target_snapshot.pop("path", None)
        if omit_receipt_artifact:
            receipt_artifacts.pop(target_id, None)
        if omit_snapshot_artifact:
            snapshot_artifacts.pop(target_id, None)

        artifacts = {
            aid: replace(
                spec,
                path=(
                    "$UNRESOLVED/bin/realmheart"
                    if aid == target_id and unresolved_manifest_path
                    else canonical_paths[aid]
                ),
                required=(False if aid == target_id and optional else spec.required),
            )
            for aid, spec in self.registry.artifacts.items()
        }
        registry = replace(self.registry, artifacts=artifacts, capabilities={})
        if legacy_snapshot_schema:
            snapshot_payload["schema_version"] = 1

        receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
        snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")
        return analyze_forensics(
            registry,
            load_installed_receipt(receipt_path),
            load_health_snapshot(snapshot_path),
        )

    def test_receipt_artifact_path_redirect_is_not_hidden_by_matching_snapshot_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                receipt_path_override="/tmp/unauthorized-receipt-target",
            )

            drifts = [
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_DRIFT"
            ]
            self.assertEqual(len(drifts), 1)
            self.assertEqual(drifts[0].severity, "critical")
            self.assertEqual(report.repair_readiness, ReadinessState.FAILED)

    def test_snapshot_artifact_path_redirect_is_not_hidden_by_matching_receipt_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                snapshot_path_override="/tmp/unauthorized-snapshot-target",
            )

            drifts = [
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_DRIFT"
            ]
            self.assertEqual(len(drifts), 1)
            self.assertEqual(drifts[0].severity, "critical")
            self.assertEqual(report.repair_readiness, ReadinessState.FAILED)

    def test_missing_snapshot_artifact_path_is_explicitly_uncertain(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(Path(temp), omit_snapshot_path=True)

            drifts = [
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"
            ]
            self.assertEqual(len(drifts), 1)
            self.assertEqual(drifts[0].severity, "warning")
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)

    def test_unresolved_manifest_artifact_token_is_not_authorized(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(Path(temp), unresolved_manifest_path=True)

            drifts = [
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"
            ]
            self.assertGreaterEqual(len(drifts), 1)
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)

    def test_valid_canonical_artifact_path_is_clean_for_path_binding(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(Path(temp))

            self.assertFalse(any(
                item.subject_id == "core.binary"
                and "PATH_" in item.error_code
                for item in report.drifts
            ))

    def test_optional_artifact_path_redirect_is_warning_and_degraded(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                snapshot_path_override="/tmp/unauthorized-optional-target",
                optional=True,
            )

            drift = next(
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_DRIFT"
            )
            self.assertEqual(drift.severity, "warning")
            self.assertEqual(report.repair_readiness, ReadinessState.DEGRADED)

    def test_snapshot_path_is_checked_even_when_receipt_omits_artifact_record(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                snapshot_path_override="/tmp/unauthorized-without-receipt-target",
                omit_receipt_artifact=True,
            )

            drift = next(
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_DRIFT"
            )
            self.assertEqual(drift.severity, "critical")
            self.assertEqual(report.repair_readiness, ReadinessState.FAILED)

    def test_required_artifact_omitted_from_both_sources_is_one_path_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                omit_receipt_artifact=True,
                omit_snapshot_artifact=True,
            )

            drifts = [item for item in report.drifts if item.subject_id == "core.binary"]
            self.assertEqual(
                [item.error_code for item in drifts],
                ["RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"],
            )
            self.assertTrue(drifts[0].affects_repair)
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)

    def test_required_receipt_only_artifact_is_path_unknown_and_not_duplicate_generic_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                omit_snapshot_artifact=True,
            )

            drifts = [item for item in report.drifts if item.subject_id == "core.binary"]
            self.assertEqual(
                [item.error_code for item in drifts],
                ["RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"],
            )
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)

    def test_required_snapshot_only_artifact_is_path_unknown_and_not_duplicate_generic_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                omit_receipt_artifact=True,
            )

            drifts = [item for item in report.drifts if item.subject_id == "core.binary"]
            self.assertEqual(
                [item.error_code for item in drifts],
                ["RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"],
            )
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)

    def test_optional_artifact_omitted_from_both_sources_degrades_without_required_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(
                Path(temp),
                optional=True,
                omit_receipt_artifact=True,
                omit_snapshot_artifact=True,
            )

            drifts = [item for item in report.drifts if item.subject_id == "core.binary"]
            self.assertEqual(
                [item.error_code for item in drifts],
                ["RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"],
            )
            self.assertEqual(drifts[0].severity, "warning")
            self.assertEqual(report.repair_readiness, ReadinessState.DEGRADED)

    def test_complete_canonical_artifact_coverage_keeps_repair_readiness_healthy(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(Path(temp))

            self.assertFalse(any(item.kind is DriftKind.ARTIFACT for item in report.drifts))
            self.assertEqual(report.repair_readiness, ReadinessState.HEALTHY)

    def test_legacy_snapshot_schema_is_loaded_but_path_uncertain(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._path_binding_case(Path(temp), legacy_snapshot_schema=True)

            self.assertTrue(any(
                item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"
                for item in report.drifts
            ))
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)

    def test_snapshot_serializer_preserves_observed_artifact_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            receipt_path, receipt_payload = self._generated_receipt(Path(temp))
            snapshot_path, _ = self._snapshot_from_receipt(Path(temp), receipt_payload)
            snapshot = load_health_snapshot(snapshot_path)

            serialized = serialize_health_snapshot(snapshot)
            artifacts = serialized["artifacts"]
            if not isinstance(artifacts, dict):
                self.fail("serialized artifact collection must be an object")
            serialized_artifact = artifacts["core.binary"]
            if not isinstance(serialized_artifact, dict):
                self.fail("serialized core.binary observation must be an object")
            self.assertEqual(
                serialized_artifact["path"],
                load_installed_receipt(receipt_path).artifacts["core.binary"].path,
            )

    def _report_for_capability_state(
        self,
        root: Path,
        *,
        capability_id: str,
        accepted_state: str,
        accepted_version: str | None,
        current_state: str,
        current_version: str | None,
    ) -> ForensicReport:
        receipt_path, receipt_payload = self._generated_receipt(root)
        dependencies = receipt_payload["dependencies"]
        if not isinstance(dependencies, dict):
            self.fail("generated receipt dependencies must be an object")
        accepted = dependencies[capability_id]
        if not isinstance(accepted, dict):
            self.fail(f"generated {capability_id} receipt must be an object")
        accepted.update(state=accepted_state, version=accepted_version)
        receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")

        snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
        capabilities = snapshot_payload["capabilities"]
        if not isinstance(capabilities, dict):
            self.fail("synthetic snapshot capabilities must be an object")
        current = capabilities[capability_id]
        if not isinstance(current, dict):
            self.fail(f"synthetic {capability_id} observation must be an object")
        current.update(state=current_state, version=current_version)
        snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

        return analyze_forensics(
            self.registry,
            load_installed_receipt(receipt_path),
            load_health_snapshot(snapshot_path),
        )

    def test_generated_installer_receipt_supports_three_way_dependency_and_artifact_diff(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            snapshot_payload["capabilities"]["runtime.dbus-update-environment"]["state"] = "missing"
            snapshot_payload["artifacts"]["core.binary"]["sha256"] = "0" * 64
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )
            kinds = {item.kind for item in report.drifts}
            self.assertIn(DriftKind.DEPENDENCY, kinds)
            self.assertIn(DriftKind.ARTIFACT, kinds)
            self.assertTrue(any(item.error_code == "RH_FORENSIC_DEPENDENCY_MISSING" for item in report.drifts))
            self.assertTrue(any(item.error_code == "RH_FORENSIC_ARTIFACT_HASH_DRIFT" for item in report.drifts))

    def test_incompatible_current_version_emits_one_canonical_forensic_finding(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            receipt_payload["dependencies"]["build.cmake"]["version"] = "3.26.0"
            capabilities = snapshot_payload["capabilities"]
            if not isinstance(capabilities, dict):
                self.fail("synthetic snapshot capabilities must be an object")
            build_cmake = capabilities["build.cmake"]
            if not isinstance(build_cmake, dict):
                self.fail("synthetic build.cmake observation must be an object")
            build_cmake.update(state="pass", version="2.0.0")
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            findings = [
                item for item in report.drifts
                if item.subject_id == "build.cmake"
            ]
            self.assertEqual(
                [(item.error_code, item.severity) for item in findings],
                [("RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE", "error")],
            )

    def test_required_pass_observation_without_version_is_explicit_unknown_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            dependencies = receipt_payload["dependencies"]
            if not isinstance(dependencies, dict):
                self.fail("generated receipt dependencies must be an object")
            build_cmake_receipt = dependencies["build.cmake"]
            if not isinstance(build_cmake_receipt, dict):
                self.fail("generated build.cmake receipt must be an object")
            build_cmake_receipt["version"] = "3.30.0"
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            capabilities = snapshot_payload["capabilities"]
            if not isinstance(capabilities, dict):
                self.fail("synthetic snapshot capabilities must be an object")
            build_cmake = capabilities["build.cmake"]
            if not isinstance(build_cmake, dict):
                self.fail("synthetic build.cmake observation must be an object")
            build_cmake.update(state="pass", version=None)
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            version_unknown = [
                item for item in report.drifts
                if item.subject_id == "build.cmake"
                and item.error_code == "RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN"
            ]
            dependency_unknown = [
                item for item in report.drifts
                if item.subject_id == "build.cmake"
                and item.error_code == "RH_FORENSIC_DEPENDENCY_UNKNOWN"
            ]
            self.assertEqual(len(version_unknown), 1)
            self.assertEqual(version_unknown[0].severity, "warning")
            self.assertEqual(len(dependency_unknown), 1)
            self.assertEqual(dependency_unknown[0].current, "unknown")
            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)
            self.assertTrue(report.has_drift)

    def test_failed_observation_preserves_failure_when_version_is_unparseable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            dependencies = receipt_payload["dependencies"]
            if not isinstance(dependencies, dict):
                self.fail("generated receipt dependencies must be an object")
            python3_receipt = dependencies["runtime.python3"]
            if not isinstance(python3_receipt, dict):
                self.fail("generated runtime.python3 receipt must be an object")
            python3_receipt["version"] = "3.12.0"
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            capabilities = snapshot_payload["capabilities"]
            if not isinstance(capabilities, dict):
                self.fail("synthetic snapshot capabilities must be an object")
            python3 = capabilities["runtime.python3"]
            if not isinstance(python3, dict):
                self.fail("synthetic runtime.python3 observation must be an object")
            python3.update(state="failed", version="not-a-version")
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            findings = [
                item for item in report.drifts
                if item.subject_id == "runtime.python3"
            ]
            self.assertEqual(
                [(item.error_code, item.severity, item.current) for item in findings],
                [
                    ("RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN", "warning", "not-a-version"),
                    ("RH_FORENSIC_DEPENDENCY_FAILED", "critical", "failed"),
                ],
            )
            self.assertIn("regressed from pass to failed", findings[1].summary)
            self.assertNotIn("state is pass", findings[0].summary)

    def test_current_failed_state_is_not_gated_by_unhealthy_accepted_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._report_for_capability_state(
                Path(temp),
                capability_id="runtime.python3",
                accepted_state="missing",
                accepted_version="3.12.0",
                current_state="failed",
                current_version="not-a-version",
            )

            findings = [item for item in report.drifts if item.subject_id == "runtime.python3"]
            self.assertEqual(
                [(item.error_code, item.severity, item.current) for item in findings],
                [
                    ("RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN", "warning", "not-a-version"),
                    ("RH_FORENSIC_DEPENDENCY_FAILED", "critical", "failed"),
                ],
            )
            self.assertFalse(any(item.error_code == "RH_FORENSIC_DEPENDENCY_STATE_DRIFT" for item in findings))

    def test_current_missing_state_classifies_unparseable_version_without_generic_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._report_for_capability_state(
                Path(temp),
                capability_id="runtime.python3",
                accepted_state="missing",
                accepted_version="3.12.0",
                current_state="missing",
                current_version="not-a-version",
            )

            findings = [item for item in report.drifts if item.subject_id == "runtime.python3"]
            self.assertEqual(
                [(item.error_code, item.severity, item.current) for item in findings],
                [
                    ("RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN", "warning", "not-a-version"),
                    ("RH_FORENSIC_DEPENDENCY_MISSING", "critical", "missing"),
                ],
            )
            self.assertFalse(any(item.error_code == "RH_FORENSIC_DEPENDENCY_VERSION_DRIFT" for item in findings))
            self.assertTrue(report.has_drift)

    def test_current_missing_state_classifies_incompatible_version_without_duplicate(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._report_for_capability_state(
                Path(temp),
                capability_id="runtime.hyprctl",
                accepted_state="missing",
                accepted_version="hyprctl 0.57.0",
                current_state="missing",
                current_version="hyprctl 0.55.0",
            )

            findings = [item for item in report.drifts if item.subject_id == "runtime.hyprctl"]
            self.assertEqual(
                [(item.error_code, item.severity, item.current) for item in findings],
                [
                    ("RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE", "critical", "hyprctl 0.55.0"),
                    ("RH_FORENSIC_DEPENDENCY_MISSING", "critical", "missing"),
                ],
            )
            self.assertEqual(
                sum(item.error_code == "RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE" for item in findings),
                1,
            )
            self.assertFalse(any(item.error_code == "RH_FORENSIC_DEPENDENCY_VERSION_DRIFT" for item in findings))

    def test_current_healthy_pass_is_not_reported_as_failure_after_unhealthy_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            report = self._report_for_capability_state(
                Path(temp),
                capability_id="runtime.python3",
                accepted_state="missing",
                accepted_version="3.12.0",
                current_state="pass",
                current_version="3.12.0",
            )

            findings = [item for item in report.drifts if item.subject_id == "runtime.python3"]
            self.assertEqual(
                [(item.error_code, item.severity, item.current) for item in findings],
                [("RH_FORENSIC_DEPENDENCY_STATE_DRIFT", "warning", "pass")],
            )
            self.assertFalse(
                any(
                    item.error_code
                    in {
                        "RH_FORENSIC_DEPENDENCY_MISSING",
                        "RH_FORENSIC_DEPENDENCY_FAILED",
                    }
                    for item in findings
                )
            )


    def test_one_dependency_root_collapses_multiple_capabilities_and_dependents(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            for capid in ("runtime.dbus-update-environment", "verification.dbus-run-session"):
                snapshot_payload["capabilities"][capid]["state"] = "missing"
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))
            dbus = [item for item in report.incidents if item.root_kind == "dependency" and item.root_id == "dep.dbus"]
            self.assertEqual(len(dbus), 1)
            self.assertEqual(
                set(dbus[0].capability_ids),
                {"runtime.dbus-update-environment", "verification.dbus-run-session"},
            )
            self.assertIn("session", dbus[0].affected_components)
            self.assertIn("native-tests", dbus[0].affected_components)
            self.assertIn("hypr-integration", dbus[0].affected_components)

    def test_runtime_health_is_independent_from_build_repair_readiness(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            snapshot_payload["runtime_health"] = "healthy"
            snapshot_payload["activation_state"] = "active"
            snapshot_payload["capabilities"]["build.cmake"]["state"] = "missing"
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))
            self.assertEqual(report.runtime_health, "healthy")
            self.assertEqual(report.repair_readiness, ReadinessState.FAILED)
            build_drift = next(item for item in report.drifts if item.subject_id == "build.cmake")
            self.assertFalse(build_drift.affects_runtime)
            self.assertTrue(build_drift.affects_repair)

    def test_unknown_observation_is_uncertain_not_dependency_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            capabilities = snapshot_payload["capabilities"]
            if not isinstance(capabilities, dict):
                self.fail("synthetic snapshot capabilities must be an object")
            build_cmake = capabilities["build.cmake"]
            if not isinstance(build_cmake, dict):
                self.fail("synthetic build.cmake observation must be an object")
            build_cmake["state"] = "unknown"
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))

            self.assertEqual(report.repair_readiness, ReadinessState.UNKNOWN)
            unknown_drift = next(
                item
                for item in report.drifts
                if item.subject_id == "build.cmake"
                and item.error_code == "RH_FORENSIC_DEPENDENCY_UNKNOWN"
            )
            self.assertEqual(unknown_drift.error_code, "RH_FORENSIC_DEPENDENCY_UNKNOWN")
            self.assertEqual(unknown_drift.severity, "warning")
            self.assertNotEqual(unknown_drift.error_code, "RH_FORENSIC_DEPENDENCY_FAILED")

    def test_missing_required_runtime_observation_is_explicit_unknown_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            capabilities = snapshot_payload["capabilities"]
            if not isinstance(capabilities, dict):
                self.fail("synthetic snapshot capabilities must be an object")
            capabilities.pop("runtime.dbus-update-environment", None)
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))

            missing = next(item for item in report.drifts if item.subject_id == "runtime.dbus-update-environment")
            self.assertEqual(missing.error_code, "RH_FORENSIC_DEPENDENCY_UNKNOWN")
            self.assertEqual(missing.current, "unknown")
            self.assertTrue(report.has_drift)

    def test_missing_required_artifact_observation_is_explicit_unknown_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            artifacts = snapshot_payload["artifacts"]
            if not isinstance(artifacts, dict):
                self.fail("synthetic snapshot artifacts must be an object")
            artifacts.pop("core.binary", None)
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))

            missing = next(item for item in report.drifts if item.subject_id == "core.binary")
            self.assertEqual(missing.error_code, "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN")
            self.assertEqual(missing.current, "unknown")
            self.assertEqual(missing.severity, "warning")
            self.assertTrue(missing.affects_repair)
            self.assertTrue(report.has_drift)

    def test_missing_optional_capability_observation_preserves_optional_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            optional = next(spec for spec in self.registry.capabilities.values() if spec.requirement == "soft")
            capabilities = snapshot_payload["capabilities"]
            if not isinstance(capabilities, dict):
                self.fail("synthetic snapshot capabilities must be an object")
            capabilities.pop(optional.id, None)
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))

            self.assertFalse(any(item.subject_id == optional.id for item in report.drifts))

    def test_missing_required_receipt_records_are_explicit_unknown_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, _ = self._snapshot_from_receipt(root, receipt_payload)
            required_capability = next(
                spec.id for spec in self.registry.capabilities.values() if spec.requirement == "required"
            )
            required_artifact = next(
                spec.id for spec in self.registry.artifacts.values() if spec.required
            )
            receipt_payload["dependencies"].pop(required_capability)
            receipt_payload["artifacts"].pop(required_artifact)
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            self.assertTrue(report.has_drift)
            self.assertTrue(any(
                item.subject_id == required_capability
                and item.error_code == "RH_FORENSIC_DEPENDENCY_UNKNOWN"
                for item in report.drifts
            ))
            self.assertTrue(any(
                item.subject_id == required_artifact
                and item.error_code == "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN"
                for item in report.drifts
            ))

    def test_unknown_receipt_ids_are_explicit_contract_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, _ = self._snapshot_from_receipt(root, receipt_payload)
            known_capability = next(iter(receipt_payload["dependencies"].values()))
            known_artifact = next(iter(receipt_payload["artifacts"].values()))
            receipt_payload["dependencies"]["receipt.unknown-capability"] = dict(known_capability)
            receipt_payload["artifacts"]["receipt.unknown-artifact"] = dict(known_artifact)
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            unknown = [item for item in report.drifts if item.error_code == "RH_FORENSIC_RECEIPT_UNKNOWN_ID"]
            self.assertEqual(
                {item.subject_id for item in unknown},
                {"receipt.unknown-capability", "receipt.unknown-artifact"},
            )
            self.assertTrue(report.has_drift)

    def test_missing_optional_component_observation_is_non_nuclear(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            optional_component = next(
                spec for spec in self.registry.capabilities.values()
                if spec.requirement == "component"
                and spec.component_id is not None
                and self.registry.components[spec.component_id].category not in {"core", "essential", "fx"}
            )
            snapshot_payload["capabilities"].pop(optional_component.id)
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            drift = next(item for item in report.drifts if item.subject_id == optional_component.id)
            self.assertEqual(drift.error_code, "RH_FORENSIC_DEPENDENCY_UNKNOWN")
            self.assertEqual(drift.severity, "warning")
            self.assertFalse(any(item.severity == "critical" for item in report.drifts))

    def test_omitted_optional_receipt_record_with_matching_snapshot_is_contract_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, _ = self._snapshot_from_receipt(root, receipt_payload)
            optional = next(
                spec for spec in self.registry.capabilities.values() if spec.requirement == "soft"
            )
            receipt_payload["dependencies"].pop(optional.id)
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                load_health_snapshot(snapshot_path),
            )

            coverage = [
                item for item in report.drifts
                if item.subject_id == optional.id
                and item.error_code == "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN"
            ]
            self.assertEqual(len(coverage), 1)
            self.assertEqual(coverage[0].current, "unknown")
            self.assertEqual(coverage[0].severity, "warning")
            self.assertTrue(report.has_drift)

    def test_direct_invalid_artifact_observation_outcome_is_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            receipt = load_installed_receipt(receipt_path)
            artifact = receipt.artifacts["core.binary"]
            snapshot = CurrentHealthSnapshot(
                schema_version=2,
                captured_at="2026-09-13T12:00:00+00:00",
                activation_state=receipt.activation_state,
                runtime_health=receipt.runtime_health,
                capabilities={},
                artifacts={
                    "core.binary": ArtifactObservation(
                        artifact_id="core.binary",
                        exists=True,
                        path=artifact.path,
                        sha256=artifact.sha256,
                        immutable_fingerprint=artifact.immutable_fingerprint,
                        mode=artifact.mode,
                        filesystem_type="file",
                        outcome="not-a-valid-outcome",  # type: ignore[arg-type]
                    )
                },
            )

            report = analyze_forensics(self.registry, receipt, snapshot)

            unknown = [
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_UNKNOWN"
            ]
            self.assertTrue(unknown)
            self.assertFalse(any(
                item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_HASH_DRIFT"
                for item in report.drifts
            ))

    def test_direct_invalid_receipt_digest_is_unknown_not_hash_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, _ = self._snapshot_from_receipt(root, receipt_payload)
            receipt = load_installed_receipt(receipt_path)
            artifacts = dict(receipt.artifacts)
            artifacts["core.binary"] = replace(artifacts["core.binary"], sha256="not-a-digest")
            receipt = replace(receipt, artifacts=artifacts)

            report = analyze_forensics(self.registry, receipt, load_health_snapshot(snapshot_path))

            self.assertTrue(any(
                item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_UNKNOWN"
                for item in report.drifts
            ))
            self.assertFalse(any(
                item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_HASH_DRIFT"
                for item in report.drifts
            ))

    def test_direct_artifact_observation_without_outcome_keeps_missing_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot = CurrentHealthSnapshot(
                schema_version=2,
                captured_at="2026-09-13T12:00:00+00:00",
                activation_state=receipt_payload["activation_state"],
                runtime_health=receipt_payload["runtime_health"],
                capabilities={},
                artifacts={
                    "core.binary": ArtifactObservation(
                        "core.binary",
                        False,
                        path=load_installed_receipt(receipt_path).artifacts["core.binary"].path,
                    )
                },
            )

            report = analyze_forensics(
                self.registry,
                load_installed_receipt(receipt_path),
                snapshot,
            )

            missing = [
                item for item in report.drifts
                if item.subject_id == "core.binary"
                and item.error_code == "RH_FORENSIC_ARTIFACT_MISSING"
            ]
            self.assertEqual(len(missing), 1)

    def test_health_check_selection_honors_context_cost_and_side_effect_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            components = Path(temp) / "components"
            components.mkdir()
            (components / "fixture.toml").write_text(
                '''schema_version = 1
release_version = "0.7.8"

[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"

[[artifacts]]
id = "core.file"
component_id = "core"
path = "$HOME/.config/core/file"
type = "file"
ownership = "user"

[[health_checks]]
id = "check.bg.cheap"
component_id = "core"
check = "artifact_exists"
artifact_id = "core.file"
cost = "cheap"
side_effects = "none"
timeout_ms = 100
contexts = ["doctor_background"]

[[health_checks]]
id = "check.bg.expensive"
component_id = "core"
check = "artifact_exists"
artifact_id = "core.file"
cost = "expensive"
side_effects = "none"
timeout_ms = 100
contexts = ["doctor_background"]

[[health_checks]]
id = "check.manual.cheap"
component_id = "core"
check = "artifact_exists"
artifact_id = "core.file"
cost = "cheap"
side_effects = "none"
timeout_ms = 100
contexts = ["doctor_manual"]

[[health_checks]]
id = "check.bg.side-effect"
component_id = "core"
check = "process_start_smoke"
artifact_id = "core.file"
cost = "cheap"
side_effects = "starts_component"
timeout_ms = 100
contexts = ["doctor_background"]
''',
                encoding="utf-8",
            )
            registry = load_manifest(components)
            self.assertEqual(select_health_checks(registry, context="doctor_background"), ("check.bg.cheap",))
            self.assertEqual(
                select_health_checks(registry, context="doctor_background", max_cost="expensive"),
                ("check.bg.cheap", "check.bg.expensive"),
            )
            self.assertEqual(select_health_checks(registry, context="doctor_manual"), ("check.manual.cheap",))

    def test_canonical_background_policy_selects_declared_doctor_checks(self) -> None:
        selected = select_health_checks(self.registry, context="doctor_background", max_cost="cheap")
        self.assertEqual(len(selected), len(self.registry.health_checks))
        self.assertEqual(set(selected), set(self.registry.health_checks))

    def test_manifest_identity_drift_is_explicit_not_a_parse_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, _ = self._snapshot_from_receipt(root, receipt_payload)
            receipt_payload["manifest_set_sha256"] = "f" * 64
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
            report = analyze_forensics(self.registry, load_installed_receipt(receipt_path), load_health_snapshot(snapshot_path))
            self.assertTrue(any(item.error_code == "RH_FORENSIC_MANIFEST_IDENTITY_DRIFT" for item in report.drifts))

    def test_future_receipt_schema_and_symlink_input_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            receipt_payload["schema_version"] = 999
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
            with self.assertRaisesRegex(ForensicContractError, "newer"):
                load_installed_receipt(receipt_path)
            target = root / "target.json"
            target.write_text("{}", encoding="utf-8")
            link = root / "link.json"
            link.symlink_to(target)
            with self.assertRaisesRegex(ForensicContractError, "symlink"):
                load_installed_receipt(link)


    def test_persisted_vocabulary_is_validated_instead_of_silently_extended(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            capid = next(iter(receipt_payload["dependencies"]))
            receipt_payload["dependencies"][capid]["state"] = "banana"
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")
            with self.assertRaisesRegex(ForensicContractError, "invalid state"):
                load_installed_receipt(receipt_path)

    def test_receipt_component_health_rejects_noncanonical_state(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            components = receipt_payload["components"]
            if not isinstance(components, dict):
                self.fail("generated receipt components must be an object")
            component_id = next(iter(components))
            component = components[component_id]
            if not isinstance(component, dict):
                self.fail("generated receipt component must be an object")
            component["health"] = "bogus"
            receipt_path.write_text(json.dumps(receipt_payload), encoding="utf-8")

            with self.assertRaisesRegex(ForensicContractError, "invalid health"):
                load_installed_receipt(receipt_path)

    def test_standalone_consumer_runs_without_loading_installer_package(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, receipt_payload = self._generated_receipt(root)
            snapshot_path, snapshot_payload = self._snapshot_from_receipt(root, receipt_payload)
            snapshot_payload["capabilities"]["runtime.dbus-update-environment"]["state"] = "missing"
            snapshot_path.write_text(json.dumps(snapshot_payload), encoding="utf-8")
            code = f'''
import runpy, sys
sys.argv = [{str(TOOL)!r}, "--components", {str(_bootstrap.REPO_ROOT / "components")!r}, "--receipt", {str(receipt_path)!r}, "--snapshot", {str(snapshot_path)!r}, "--json"]
try:
    runpy.run_path({str(TOOL)!r}, run_name="__main__")
except SystemExit as exc:
    assert exc.code == 0, exc.code
bad = [name for name in sys.modules if name == "realmheart_installer" or name.startswith("realmheart_installer.")]
assert not bad, bad
'''
            proc = subprocess.run([sys.executable, "-c", code], cwd=_bootstrap.REPO_ROOT, text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            payload = json.loads(proc.stdout)
            self.assertTrue(any(item["root_id"] == "dep.dbus" for item in payload["incidents"]))

    def test_tool_rejects_malformed_snapshot_with_stable_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt_path, _ = self._generated_receipt(root)
            snapshot = root / "bad.json"
            snapshot.write_text('{"schema_version": 999}', encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "--receipt", str(receipt_path), "--snapshot", str(snapshot)],
                cwd=_bootstrap.REPO_ROOT,
                text=True,
                capture_output=True,
            )
            self.assertEqual(proc.returncode, 30)
            self.assertIn("Forensic contract error", proc.stderr)


if __name__ == "__main__":
    unittest.main()
