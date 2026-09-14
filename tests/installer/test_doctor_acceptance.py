from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap
from realmheart_doctor import AcceptanceAssessment, AcceptanceRecommendation, assess_candidate_install
from realmheart_doctor.acceptance import DoctorAcceptanceError, _probe_capability, load_candidate_bundle
from realmheart_doctor.cli import main as doctor_main
from realmheart_maintenance.forensics import ForensicContractError
from realmheart_maintenance.fingerprint import (
    FingerprintLimitExceeded,
    MAX_FINGERPRINT_BYTES,
    MAX_FINGERPRINT_DEPTH,
    MAX_FINGERPRINT_ENTRIES,
    MAX_FINGERPRINT_SECONDS,
    fingerprint_path,
)
from realmheart_maintenance.manifest import ManifestError, load_manifest
from realmheart_installer.finalization import build_final_decision, build_installed_state_receipt
from realmheart_installer.finalization.models import FinalAction, FinalSeverity
from realmheart_installer.models import InstallMode
from realmheart_installer.verification.models import InstallHealthState

TOOL = _bootstrap.REPO_ROOT / "tools/realmheart-doctor.py"


def _manifest(
    root: Path,
    *,
    category: str = "core",
    capability_requirement: str = "required",
    probe_version: bool = False,
    minimum_version: str | None = None,
    probe_minimum_version: str | None = None,
    artifact_template: str | None = None,
    artifact_type: str = "file",
):
    components = root / "components"
    components.mkdir()
    artifact = root / "installed-demo"
    artifact_path = artifact_template or str(artifact)
    body = f'''schema_version = 1
release_version = "0.7.8"

[[components]]
id = "demo"
name = "Demo"
component_version = "release"
category = "{category}"
stage = "foundation"

[[external_dependencies]]
id = "dep.python"
name = "Python"
{(f'minimum_version = "{minimum_version}"' if minimum_version else '')}

[[capabilities]]
id = "runtime.python"
dependency_id = "dep.python"
display_name = "Python"
requirement = "{capability_requirement}"
lifecycle = ["runtime"]
component_id = "demo"
[capabilities.probe]
kind = "executable"
executable = "python3"
{('version_argv = ["--version"]' if probe_version else '')}
{(f'minimum_version = "{probe_minimum_version}"' if probe_minimum_version else '')}

[[artifacts]]
id = "demo.file"
component_id = "demo"
path = "{artifact_path}"
type = "{artifact_type}"
required = true
ownership = "release"
managed = true
'''
    (components / "demo.toml").write_text(body)
    return load_manifest(components), artifact


def _candidate(registry, artifact: Path, *, install_health="healthy", activation="active", runtime="healthy"):
    exists = artifact.exists() or artifact.is_symlink()
    mode = None
    sha256 = None
    immutable_fingerprint = None
    if exists:
        observed = artifact.lstat()
        mode = "0644"
        if stat.S_ISREG(observed.st_mode) and not stat.S_ISLNK(observed.st_mode):
            sha256 = hashlib.sha256(artifact.read_bytes()).hexdigest()
        immutable_fingerprint = fingerprint_path(artifact)
    return {
        "schema_version": 1,
        "kind": "realmheart_install_candidate",
        "realmheart_version": "0.7.8",
        "manifest_schema_version": registry.schema_version,
        "manifest_set_sha256": registry.digest,
        "installer_version": "0.7.8",
        "transaction_id": "RH-DOCTOR-TEST",
        "install_health": install_health,
        "activation_state": activation,
        "runtime_health": runtime,
        "components": {
            "demo": {
                "display_name": "Demo",
                "category": registry.components["demo"].category,
                "health": "healthy" if install_health == "healthy" else install_health,
                "blocked_by": [],
                "artifact_ids": ["demo.file"],
                "build_unit_ids": [],
            }
        },
        "dependencies": {
            "runtime.python": {
                "component_id": "demo",
                "requirement": registry.capabilities["runtime.python"].requirement,
                "lifecycle": ["runtime"],
                "state": "pass",
                "version": None,
            }
        },
        "artifacts": {
            "demo.file": {
                "component_id": "demo",
                "path": str(artifact),
                "type": registry.artifacts["demo.file"].type,
                "ownership": "release",
                "mode": mode,
                "sha256": sha256,
                "immutable_fingerprint": immutable_fingerprint,
            }
        },
    }


class DoctorAcceptanceTests(unittest.TestCase):
    def test_healthy_candidate_is_keep(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root); artifact.write_text("ok\n"); artifact.chmod(0o644)
            result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.KEEP)
            self.assertEqual(result.checked_artifacts,1)
            self.assertEqual(result.checked_capabilities,1)

    def test_missing_core_artifact_recommends_revert(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root)
            result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(x.code=="RH_FORENSIC_ARTIFACT_MISSING" for x in result.findings))

    def test_installer_failed_candidate_cannot_be_greenlit(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root); artifact.write_text("ok\n"); artifact.chmod(0o644)
            result=assess_candidate_install(registry,_candidate(registry,artifact,install_health="failed",runtime="failed"))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.REVERT_RECOMMENDED)

    def test_pending_session_restart_is_not_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root); artifact.write_text("ok\n"); artifact.chmod(0o644)
            result=assess_candidate_install(registry,_candidate(registry,artifact,activation="pending_session_restart",runtime="healthy"))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.KEEP)
            self.assertTrue(any(x.code=="RH_DOCTOR_RUNTIME_PENDING_SESSION_RESTART" for x in result.findings))

    def test_noncore_runtime_failure_warns_instead_of_reverting(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root,category="qol"); artifact.write_text("ok\n"); artifact.chmod(0o644)
            with patch("realmheart_doctor.acceptance.shutil.which", return_value=None):
                result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.KEEP_WITH_WARNINGS)

    def test_essential_runtime_dependency_failure_recommends_revert(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root,category="essential"); artifact.write_text("ok\n"); artifact.chmod(0o644)
            with patch("realmheart_doctor.acceptance.shutil.which", return_value=None):
                result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(x.code=="RH_FORENSIC_DEPENDENCY_MISSING" and x.severity=="critical" for x in result.findings))

    def test_required_essential_artifact_missing_recommends_revert(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root,category="essential")
            result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(x.code=="RH_FORENSIC_ARTIFACT_MISSING" and x.severity=="critical" for x in result.findings))

    def test_required_core_artifact_hash_drift_recommends_revert(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root); artifact.write_text("before\n"); artifact.chmod(0o644)
            payload=_candidate(registry,artifact)
            payload["artifacts"]["demo.file"]["sha256"] = hashlib.sha256(b"before\n").hexdigest()
            artifact.write_text("after\n")
            result=assess_candidate_install(registry,payload)
            self.assertEqual(result.recommendation,AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(x.code=="RH_FORENSIC_ARTIFACT_HASH_DRIFT" and x.severity=="critical" for x in result.findings))

    def test_required_core_artifact_mode_drift_recommends_revert(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root); artifact.write_text("ok\n"); artifact.chmod(0o600)
            result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(x.code=="RH_FORENSIC_ARTIFACT_MODE_DRIFT" and x.severity=="critical" for x in result.findings))

    def test_required_managed_artifact_without_integrity_is_indeterminate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            artifact.write_text("ok\n")
            payload = _candidate(registry, artifact)
            payload["artifacts"]["demo.file"].update(
                mode=None,
                sha256=None,
                immutable_fingerprint=None,
            )

            result = assess_candidate_install(registry, payload)

            self.assertEqual(result.recommendation, AcceptanceRecommendation.INDETERMINATE)
            self.assertTrue(any(item.code == "RH_DOCTOR_ARTIFACT_INTEGRITY_UNKNOWN" for item in result.findings))

    def test_artifact_symlink_is_observed_as_type_drift(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            target = root / "target"
            target.write_text("ok\n")
            artifact.symlink_to(target)

            result = assess_candidate_install(registry, _candidate(registry, artifact))

            self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(item.code == "RH_FORENSIC_ARTIFACT_TYPE_DRIFT" for item in result.findings))

    def test_executable_artifact_without_executable_mode_is_unhealthy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root, artifact_type="executable")
            artifact.write_text("#!/bin/sh\nexit 0\n")
            artifact.chmod(0o644)

            result = assess_candidate_install(registry, _candidate(registry, artifact))

            self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(item.code == "RH_FORENSIC_ARTIFACT_MODE_DRIFT" for item in result.findings))

    def test_successful_empty_or_malformed_version_is_indeterminate(self):
        for output in ("", "not a version", "not a version 3.12.0"):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                registry, artifact = _manifest(root, probe_version=True)
                artifact.write_text("ok\n")
                payload = _candidate(registry, artifact)
                completed = subprocess.CompletedProcess(
                    ("python3", "--version"), 0, stdout=output, stderr=""
                )
                with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"), patch(
                    "realmheart_doctor.acceptance._run", return_value=completed
                ):
                    result = assess_candidate_install(registry, payload)

                self.assertEqual(result.recommendation, AcceptanceRecommendation.INDETERMINATE)
                self.assertTrue(any(item.code == "RH_DOCTOR_CAPABILITY_VERSION_UNKNOWN" for item in result.findings))

    def test_version_probe_without_parseable_evidence_is_unknown_at_observation_boundary(self):
        for output in (
            "not a version 3.12.0\n",
            "3.12.0 arbitrary text\n",
            "not python 3.12.0\n",
        ):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                registry, _ = _manifest(root, probe_version=True)
                spec = registry.capabilities["runtime.python"]
                completed = subprocess.CompletedProcess(
                    ("python3", "--version"), 0, stdout=output, stderr=""
                )
                with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"), patch(
                    "realmheart_doctor.acceptance._run", return_value=completed
                ):
                    observation = _probe_capability(spec)

                self.assertEqual(observation.state, "unknown")
                self.assertIsNone(observation.version)

    def test_declared_minimum_version_mismatch_recommends_revert(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root, probe_version=True, minimum_version="99.0.0")
            artifact.write_text("ok\n")
            payload = _candidate(registry, artifact)
            payload["dependencies"]["runtime.python"]["version"] = "Python 3.12.0"
            completed = subprocess.CompletedProcess(
                ("python3", "--version"), 0, stdout="Python 3.12.0\n", stderr=""
            )
            with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"), patch(
                "realmheart_doctor.acceptance._run", return_value=completed
            ):
                result = assess_candidate_install(registry, payload)

            self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(item.code == "RH_FORENSIC_DEPENDENCY_FAILED" for item in result.findings))

    def test_dependency_and_probe_version_constraints_are_both_enforced(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(
                root,
                probe_version=True,
                minimum_version="3.0.0",
                probe_minimum_version="99.0.0",
            )
            artifact.write_text("ok\n")
            payload = _candidate(registry, artifact)
            payload["dependencies"]["runtime.python"]["version"] = "Python 3.12.0"
            completed = subprocess.CompletedProcess(
                ("python3", "--version"), 0, stdout="Python 3.12.0\n", stderr=""
            )
            with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"), patch(
                "realmheart_doctor.acceptance._run", return_value=completed
            ):
                result = assess_candidate_install(registry, payload)

            self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(item.code == "RH_FORENSIC_DEPENDENCY_FAILED" for item in result.findings))

    def test_component_requirement_on_blocking_component_is_critical(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root, category="essential", capability_requirement="component")
            artifact.write_text("ok\n")
            with patch("realmheart_doctor.acceptance.shutil.which", return_value=None):
                result = assess_candidate_install(registry, _candidate(registry, artifact))

            self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(item.code == "RH_FORENSIC_DEPENDENCY_MISSING" and item.severity == "critical" for item in result.findings))

    def test_component_requirement_on_qol_component_remains_warning(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root, category="qol", capability_requirement="component")
            artifact.write_text("ok\n")
            with patch("realmheart_doctor.acceptance.shutil.which", return_value=None):
                result = assess_candidate_install(registry, _candidate(registry, artifact))

            self.assertEqual(result.recommendation, AcceptanceRecommendation.KEEP_WITH_WARNINGS)
            self.assertTrue(any(item.code == "RH_FORENSIC_DEPENDENCY_MISSING" and item.severity == "warning" for item in result.findings))

    def test_failed_candidate_capability_cannot_be_greenlit_by_current_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            artifact.write_text("ok\n")
            payload = _candidate(registry, artifact)
            payload["dependencies"]["runtime.python"]["state"] = "failed"

            with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"):
                result = assess_candidate_install(registry, payload)

            self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)
            self.assertTrue(any(
                item.code == "RH_DOCTOR_CAPABILITY_UNHEALTHY"
                and item.subject == "runtime.python"
                and item.severity == "critical"
                for item in result.findings
            ))

    def test_blocking_capability_not_applicable_is_indeterminate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            artifact.write_text("ok\n")
            payload = _candidate(registry, artifact)
            payload["dependencies"]["runtime.python"]["state"] = "not_applicable"

            with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"):
                result = assess_candidate_install(registry, payload)

            self.assertEqual(result.recommendation, AcceptanceRecommendation.INDETERMINATE)
            self.assertTrue(any(
                item.code == "RH_DOCTOR_CAPABILITY_UNCERTAIN"
                and item.subject == "runtime.python"
                for item in result.findings
            ))

    def test_blocking_component_unestablished_health_cannot_pass(self):
        uncertain_states = ("unknown", "pending", "running", "skipped", "not_applicable", "pending_activation")
        for state in uncertain_states:
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                registry, artifact = _manifest(root)
                artifact.write_text("ok\n")
                payload = _candidate(registry, artifact)
                payload["components"]["demo"]["health"] = state

                result = assess_candidate_install(registry, payload)

                self.assertEqual(result.recommendation, AcceptanceRecommendation.INDETERMINATE)
                self.assertTrue(any(item.code == "RH_DOCTOR_COMPONENT_UNCERTAIN" for item in result.findings))

    def test_blocking_component_failed_or_blocked_health_recommends_revert(self):
        for state in ("failed", "blocked"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                registry, artifact = _manifest(root)
                artifact.write_text("ok\n")
                payload = _candidate(registry, artifact)
                payload["components"]["demo"]["health"] = state

                result = assess_candidate_install(registry, payload)

                self.assertEqual(result.recommendation, AcceptanceRecommendation.REVERT_RECOMMENDED)

    def test_top_level_runtime_and_activation_failures_participate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            artifact.write_text("ok\n")

            unknown = _candidate(registry, artifact, runtime="unknown")
            self.assertEqual(
                assess_candidate_install(registry, unknown).recommendation,
                AcceptanceRecommendation.INDETERMINATE,
            )

            failed = _candidate(registry, artifact, runtime="failed", activation="failed")
            self.assertEqual(
                assess_candidate_install(registry, failed).recommendation,
                AcceptanceRecommendation.REVERT_RECOMMENDED,
            )

    def test_unresolved_artifact_template_is_rejected_without_prefix_authorization(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root, artifact_template="$PREFIX/bin/demo")
            artifact.write_text("ok\n")
            payload = _candidate(registry, artifact)
            payload["artifacts"]["demo.file"]["path"] = str(root / "arbitrary" / "bin" / "demo")
            with patch.dict(os.environ, {"PREFIX": ""}, clear=False):
                with self.assertRaisesRegex(DoctorAcceptanceError, "authorized"):
                    assess_candidate_install(registry, payload)

    def test_candidate_json_and_fingerprint_limits_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            candidate = root / "candidate.json"
            candidate.write_text(json.dumps({"padding": "x" * (2 * 1024 * 1024)}) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(DoctorAcceptanceError, "limit|size"):
                load_candidate_bundle(candidate)

            large = root / "large"
            large.write_bytes(b"0123456789")
            with self.assertRaises(FingerprintLimitExceeded):
                fingerprint_path(large, max_bytes=4)

    def test_resource_limit_overrides_cannot_remove_hard_bounds(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            candidate = root / "candidate.json"
            candidate.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(DoctorAcceptanceError, "hard|limit"):
                load_candidate_bundle(candidate, max_bytes=2 * 1024 * 1024 + 1)

            artifact = root / "artifact"
            artifact.write_text("ok\n", encoding="utf-8")
            kwargs_list: tuple[dict[str, int | float], ...] = (
                {"max_entries": MAX_FINGERPRINT_ENTRIES + 1},
                {"max_depth": MAX_FINGERPRINT_DEPTH + 1},
                {"max_bytes": MAX_FINGERPRINT_BYTES + 1},
                {"max_seconds": MAX_FINGERPRINT_SECONDS + 1},
            )
            for kwargs in kwargs_list:
                with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ValueError, "hard|limit"):
                    fingerprint_path(artifact, **kwargs)

    def test_standalone_cli_expected_errors_keep_stable_json_shape(self):
        for error in (ManifestError("bad manifest"), ForensicContractError("bad forensic input"), DoctorAcceptanceError("bad candidate")):
            with self.subTest(error=type(error).__name__):
                output=StringIO()
                with patch("realmheart_doctor.cli.load_manifest", side_effect=error), redirect_stdout(output):
                    code=doctor_main(["assess-install", "--candidate", "missing.json", "--manifest-dir", "missing-components", "--json"])
                self.assertEqual(code,3)
                self.assertEqual(json.loads(output.getvalue()), {"error": str(error), "recommendation": "indeterminate"})

    def test_candidate_rejects_canonical_identity_and_coverage_tampering(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            cases = {
                "component category": lambda payload: payload["components"]["demo"].update(category="qol"),
                "component artifact coverage": lambda payload: payload["components"]["demo"].update(artifact_ids=[]),
                "required capability omission": lambda payload: payload["dependencies"].pop("runtime.python"),
                "required artifact omission": lambda payload: payload["artifacts"].pop("demo.file"),
                "artifact requiredness relabel": lambda payload: payload["artifacts"]["demo.file"].update(required=False),
                "artifact path redirection": lambda payload: payload["artifacts"]["demo.file"].update(path="/bin/sh"),
            }
            for label, mutate in cases.items():
                with self.subTest(label=label):
                    payload = _candidate(registry, artifact)
                    mutate(payload)
                    with self.assertRaisesRegex(DoctorAcceptanceError, "canonical|required|path"):
                        assess_candidate_install(registry, payload)

    def test_probe_timeout_is_indeterminate_not_critical_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root, probe_version=True)
            artifact.write_text("ok\n")
            artifact.chmod(0o644)
            with patch("realmheart_doctor.acceptance.shutil.which", return_value="/usr/bin/python3"), patch(
                "realmheart_doctor.acceptance._run", return_value=None
            ):
                result = assess_candidate_install(registry, _candidate(registry, artifact))
            self.assertEqual(result.recommendation, AcceptanceRecommendation.INDETERMINATE)
            self.assertTrue(any(item.code == "RH_DOCTOR_CAPABILITY_UNCERTAIN" for item in result.findings))
            self.assertFalse(any(item.code == "RH_FORENSIC_DEPENDENCY_MISSING" for item in result.findings))

    def test_artifact_permission_error_is_indeterminate_not_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            artifact.write_text("ok\n")
            artifact.chmod(0o644)
            payload = _candidate(registry, artifact)
            with patch("realmheart_doctor.acceptance.Path.lstat", side_effect=PermissionError("denied")):
                result = assess_candidate_install(registry, payload)
            self.assertEqual(result.recommendation, AcceptanceRecommendation.INDETERMINATE)
            self.assertTrue(any(item.code == "RH_FORENSIC_ARTIFACT_UNKNOWN" for item in result.findings))
            self.assertFalse(any(item.code == "RH_FORENSIC_ARTIFACT_MISSING" for item in result.findings))

    def test_malformed_nested_candidate_values_raise_contract_errors(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            for field in ("blocked_by", "artifact_ids", "build_unit_ids"):
                with self.subTest(field=field):
                    payload = _candidate(registry, artifact)
                    payload["components"]["demo"][field] = None
                    with self.assertRaisesRegex(DoctorAcceptanceError, field):
                        assess_candidate_install(registry, payload)

    def test_cli_malformed_utf8_is_stable_exit_three_json(self):
        with tempfile.TemporaryDirectory() as temp:
            candidate = Path(temp) / "invalid-utf8.json"
            candidate.write_bytes(b"{\xff\n")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "assess-install", "--candidate", str(candidate), "--manifest-dir", str(_bootstrap.REPO_ROOT / "components"), "--json"],
                cwd=_bootstrap.REPO_ROOT,
                text=True,
                capture_output=True,
            )
            self.assertEqual(proc.returncode, 3)
            self.assertNotIn("Traceback", proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["recommendation"], "indeterminate")

    def test_cli_malformed_nested_candidate_is_stable_exit_three_json(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, artifact = _manifest(root)
            payload = _candidate(registry, artifact)
            payload["components"]["demo"]["blocked_by"] = None
            candidate = root / "candidate.json"
            candidate.write_text(json.dumps(payload), encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "assess-install", "--candidate", str(candidate), "--manifest-dir", str(root / "components"), "--json"],
                cwd=_bootstrap.REPO_ROOT,
                text=True,
                capture_output=True,
            )
            self.assertEqual(proc.returncode, 3)
            self.assertNotIn("Traceback", proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["recommendation"], "indeterminate")

    def test_manifest_identity_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root); artifact.write_text("ok\n")
            payload=_candidate(registry,artifact); payload["manifest_set_sha256"]="0"*64
            with self.assertRaisesRegex(DoctorAcceptanceError,"manifest digest"):
                assess_candidate_install(registry,payload)

    def test_installed_wrapper_resolves_shipped_python_runtime(self):
        import shutil, subprocess
        with tempfile.TemporaryDirectory() as temp:
            prefix=Path(temp)/"prefix"
            (prefix/"bin").mkdir(parents=True)
            runtime=prefix/"share/realmheart/python"
            runtime.mkdir(parents=True)
            manifests=prefix/"share/realmheart/components"
            shutil.copytree(_bootstrap.REPO_ROOT/"realmheart_doctor",runtime/"realmheart_doctor")
            shutil.copytree(_bootstrap.REPO_ROOT/"realmheart_maintenance",runtime/"realmheart_maintenance",ignore=shutil.ignore_patterns("__pycache__","*.pyc"))
            shutil.copytree(_bootstrap.REPO_ROOT/"components",manifests)
            wrapper=prefix/"bin/realmheart-doctor"
            shutil.copy2(_bootstrap.REPO_ROOT/"tools/realmheart-doctor.py",wrapper)
            wrapper.chmod(0o755)
            result=subprocess.run([str(wrapper),"--version"],cwd=Path("/"),text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),"realmheart-doctor 0.7.8")

    def test_doctor_package_does_not_import_installer(self):
        import subprocess, sys
        code='import sys, realmheart_doctor; print(any(x == "realmheart_installer" or x.startswith("realmheart_installer.") for x in sys.modules))'
        result=subprocess.run([sys.executable,"-c",code],cwd=_bootstrap.REPO_ROOT,text=True,capture_output=True,check=True)
        self.assertEqual(result.stdout.strip(),"False")

    def test_doctor_revert_escalates_healthy_final_decision(self):
        from tests.installer.test_finalization import Phase16FinalizationTests, verification
        with tempfile.TemporaryDirectory() as temp:
            helper=Phase16FinalizationTests(); _, plan=helper._plan(Path(temp),InstallMode.FRESH)
            assessment=AcceptanceAssessment(1,AcceptanceRecommendation.REVERT_RECOMMENDED,plan.transaction_id,"0.7.8",plan.manifest_digest,1,1,"active","healthy",(),0,0,"critical Doctor evidence")
            decision=build_final_decision(plan,verification(),assessment)
            self.assertEqual(decision.severity,FinalSeverity.CRITICAL)
            self.assertTrue(decision.requires_explicit_choice)
            self.assertIsNone(decision.default_action)
            self.assertEqual(decision.options[0].action,FinalAction.RESTORE_PREVIOUS)

    def test_final_receipt_records_doctor_acceptance(self):
        from tests.installer.test_finalization import Phase16FinalizationTests, verification
        with tempfile.TemporaryDirectory() as temp:
            helper=Phase16FinalizationTests(); _, plan=helper._plan(Path(temp))
            report=verification(transaction_id=plan.transaction_id,manifest_digest=plan.manifest_digest,plan_digest=plan.plan_digest)
            assessment=AcceptanceAssessment(1,AcceptanceRecommendation.KEEP,plan.transaction_id,"0.7.8",plan.manifest_digest,1,1,"active","healthy",(),0,0,"looks good")
            payload=build_installed_state_receipt(plan,report,doctor_assessment=assessment)
            self.assertEqual(payload["doctor_acceptance"]["recommendation"],"keep")


if __name__ == "__main__":
    unittest.main()
