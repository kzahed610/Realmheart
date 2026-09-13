from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap
from realmheart_doctor import AcceptanceAssessment, AcceptanceRecommendation, assess_candidate_install
from realmheart_doctor.acceptance import DoctorAcceptanceError
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.finalization import build_final_decision, build_installed_state_receipt
from realmheart_installer.finalization.models import FinalAction, FinalSeverity
from realmheart_installer.models import InstallMode
from realmheart_installer.verification.models import InstallHealthState


def _manifest(root: Path, *, category: str = "core", capability_requirement: str = "required"):
    components = root / "components"
    components.mkdir()
    artifact = root / "installed-demo"
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

[[artifacts]]
id = "demo.file"
component_id = "demo"
path = "{artifact}"
type = "file"
required = true
ownership = "release"
managed = true
'''
    (components / "demo.toml").write_text(body)
    return load_manifest(components), artifact


def _candidate(registry, artifact: Path, *, install_health="healthy", activation="active", runtime="healthy"):
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
                "type": "file",
                "ownership": "release",
                "mode": "0644",
                "sha256": None,
                "immutable_fingerprint": None,
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
            result=assess_candidate_install(registry,_candidate(registry,artifact,activation="pending_session_restart",runtime="unknown"))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.KEEP)
            self.assertTrue(any(x.code=="RH_DOCTOR_RUNTIME_PENDING_SESSION_RESTART" for x in result.findings))

    def test_noncore_runtime_failure_warns_instead_of_reverting(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); registry, artifact=_manifest(root,category="qol"); artifact.write_text("ok\n"); artifact.chmod(0o644)
            with patch("realmheart_doctor.acceptance.shutil.which", return_value=None):
                result=assess_candidate_install(registry,_candidate(registry,artifact))
            self.assertEqual(result.recommendation,AcceptanceRecommendation.KEEP_WITH_WARNINGS)

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
