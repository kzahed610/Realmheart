"""Public manual Doctor contract, separate from install acceptance."""
from __future__ import annotations

from dataclasses import replace

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from realmheart_doctor.cli import main


class DoctorManualTests(unittest.TestCase):
    def invoke(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = main(list(args))
        return code, json.loads(output.getvalue())

    def test_json_reports_doctor_version_without_installer_import(self):
        import subprocess
        import sys
        result = subprocess.run(
            [sys.executable, "-B", "-c",
             'from realmheart_doctor.cli import main; import sys; '
             'main(["components", "--json"]); '
             'assert not any(k.startswith("realmheart_installer") for k in sys.modules)'],
            capture_output=True, text=True, timeout=10, check=True,
        )
        self.assertEqual(json.loads(result.stdout)["doctor_version"], "0.7.8")

    def test_explicit_state_directory_records_diagnosis_and_incident(self):
        from unittest.mock import patch
        from .test_doctor_incidents import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "state"
            with patch("realmheart_doctor.diagnosis.diagnose", return_value=_diagnosis(ComponentHealth.FAILED)):
                code, payload = self.invoke("doctor", "--state-dir", str(root), "--json")
            self.assertEqual(code, 2)
            self.assertEqual(json.loads((root / "current.json").read_text())["overall"], "failed")
            self.assertEqual(len(payload["state"]["incident_ids"]), 1)
            incident_id = payload["state"]["incident_ids"][0]
            self.assertEqual(json.loads((root / "incidents" / f"{incident_id}.json").read_text())["component_id"], "demo")

    def test_state_write_failure_keeps_diagnosis_in_clean_json(self):
        from unittest.mock import patch
        from .test_doctor_incidents import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth
        with tempfile.TemporaryDirectory() as temp:
            blocked = Path(temp) / "file"
            blocked.write_text("preserve")
            with patch("realmheart_doctor.diagnosis.diagnose", return_value=_diagnosis(ComponentHealth.FAILED)):
                code, payload = self.invoke("doctor", "--state-dir", str(blocked), "--json")
            self.assertEqual(code, 5)
            self.assertEqual(payload["overall"], "failed")
            self.assertEqual(payload["state"]["error"], "state_persistence_failed")
            self.assertEqual(blocked.read_text(), "preserve")

    def test_invalid_receipt_has_clean_json_error(self):
        with tempfile.TemporaryDirectory() as temp:
            receipt = Path(temp) / "receipt.json"
            receipt.write_text("{invalid")
            code, payload = self.invoke("doctor", "--receipt", str(receipt), "--json")
            self.assertEqual(code, 5)
            self.assertEqual(payload["error"], "receipt_configuration_error")

    def test_components_discovers_canonical_registry(self):
        code, payload = self.invoke("components", "--manifest-dir", "components", "--json")
        self.assertEqual(code, 0)
        self.assertEqual(payload["format_version"], 1)
        self.assertEqual(len(payload["components"]), 18)
        self.assertIn("lockscreen-auth", [item["id"] for item in payload["components"]])

    def test_validate_manifests_clean_error(self):
        with tempfile.TemporaryDirectory() as temp:
            code, payload = self.invoke("validate-manifests", "--manifest-dir", temp, "--json")
        self.assertEqual(code, 5)
        self.assertEqual(payload["format_version"], 1)
        self.assertEqual(payload["status"], "error")

    def test_unknown_component_is_invalid_invocation(self):
        code, payload = self.invoke("doctor", "not-a-component", "--manifest-dir", "components", "--json")
        self.assertEqual(code, 4)
        self.assertEqual(payload["status"], "error")

    def test_invalid_manual_invocation_has_versioned_json_and_exit_four(self):
        code, payload = self.invoke("doctor", "--not-an-option", "--json")
        self.assertEqual(code, 4)
        self.assertEqual(payload["format_version"], 1)
        self.assertEqual(payload["error"], "invalid_invocation")

    def test_manifest_error_does_not_echo_untrusted_source(self):
        with tempfile.TemporaryDirectory() as temp:
            Path(temp, "bad.toml").write_text('schema_version = 1\nrelease_version = "0.7.8"\n[[components]]\nid = "UNTRUSTED-SOURCE-MARKER"\n')
            code, payload = self.invoke("validate-manifests", "--manifest-dir", temp, "--json")
        self.assertEqual(code, 5)
        self.assertNotIn("UNTRUSTED-SOURCE-MARKER", json.dumps(payload))
        self.assertEqual(payload["error"], "manifest_configuration_error")

    def test_unchecked_required_artifact_prevents_healthy(self):
        from dataclasses import replace
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        registry = replace(registry, components={component.id: component},
                           component_order=(component.id,), capabilities={})
        definition = registry.health_checks["check.core.binary.exists"]
        registry = replace(registry, health_checks={definition.id: definition})
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        result = diagnose(registry, executor=executor)
        self.assertEqual(result.overall.value, "unknown")
        self.assertIn("required_artifact_coverage_missing", result.components[0].uncertainties)

    def test_explicit_install_prefix_resolves_artifacts_without_leaking_environment(self):
        import os
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "marker").write_text("present")
            (root / "manifest.toml").write_text('''schema_version = 1
release_version = "0.7.8"
[[components]]
id = "demo"
component_version = "release"
category = "core"
stage = "foundation"
[[artifacts]]
id = "demo.marker"
component_id = "demo"
path = "$PREFIX/marker"
type = "file"
required = true
ownership = "release"
managed = true
[[health_checks]]
id = "demo.exists"
component_id = "demo"
check = "artifact_exists"
artifact_id = "demo.marker"
contexts = ["doctor_manual"]
''')
            with patch.dict(os.environ, {"PREFIX": "/unrelated-original-prefix"}):
                code, payload = self.invoke("doctor", "demo", "--manifest-dir", temp,
                                            "--prefix", temp, "--json")
                self.assertEqual(os.environ["PREFIX"], "/unrelated-original-prefix")
        self.assertEqual(code, 0)
        self.assertEqual(payload["overall"], "healthy")
        self.assertEqual(payload["components"][0]["checks"][0]["status"], "pass")

    def test_component_aggregation_and_dependency_selection(self):
        from dataclasses import replace
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_maintenance.forensics import CapabilityObservation
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        registry = load_manifest(Path("components"))
        core = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        check = registry.health_checks["check.core.binary.exists"]
        assert check.artifact_id is not None
        artifact = registry.artifacts[check.artifact_id]
        registry = replace(registry, components={core.id: core}, component_order=(core.id,),
                           capabilities={}, artifacts={artifact.id: artifact}, health_checks={check.id: check})
        for category, required, observed, expected in (
            ("core", True, HealthStatus.PASS, "healthy"),
            ("core", True, HealthStatus.FAIL, "failed"),
            ("qol", True, HealthStatus.FAIL, "degraded"),
            ("core", False, HealthStatus.FAIL, "degraded"),
            ("core", True, HealthStatus.UNKNOWN, "unknown"),
            ("core", True, HealthStatus.NOT_APPLICABLE, "unknown"),
        ):
            with self.subTest(category=category, required=required, observed=observed):
                candidate = replace(registry, components={core.id: replace(core, category=category)},
                                    artifacts={artifact.id: replace(artifact, required=required)})
                executor = Mock()
                executor.execute.return_value = HealthCheckReport((HealthCheckResult(
                    check.id, core.id, check.check, observed, "fixture"),), 0)
                self.assertEqual(diagnose(candidate, executor=executor).overall.value, expected)
        registry = load_manifest(Path("components"))
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((), 0)
        capability_prober = Mock(return_value=CapabilityObservation(
            "fixture", "unknown", detail="capability probe intentionally unavailable"
        ))
        result = diagnose(
            registry,
            "lockscreen-auth",
            executor=executor,
            capability_prober=capability_prober,
        )
        self.assertEqual([item.id for item in result.components],
                         ["realmheart-core", "realmheart-fx", "lockscreen-auth"])
        self.assertEqual(result.overall.value, "unknown")
        self.assertEqual(executor.execute.call_args.kwargs["context"], "doctor_manual")

    def test_runtime_capability_observation_replaces_placeholder_uncertainty(self):
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        from realmheart_maintenance.forensics import CapabilityObservation
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        definition = registry.health_checks["check.core.binary.exists"]
        assert definition.artifact_id is not None
        artifact = registry.artifacts[definition.artifact_id]
        registry = replace(
            registry,
            components={component.id: component},
            component_order=(component.id,),
            artifacts={artifact.id: artifact},
            health_checks={definition.id: definition},
        )
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        prober = Mock(return_value=CapabilityObservation("runtime.python3", "pass", detail="python3 available"))
        result = diagnose(registry, executor=executor, capability_prober=prober)
        self.assertNotIn("runtime_capability_observation_pending", result.components[0].uncertainties)
        self.assertEqual(result.overall.value, "healthy")
        self.assertEqual(prober.call_args.kwargs["registry"], registry)

    def test_missing_required_runtime_capability_degrades_component(self):
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        from realmheart_maintenance.forensics import CapabilityObservation
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        definition = registry.health_checks["check.core.binary.exists"]
        capability = next(item for item in registry.capabilities.values()
                          if item.component_id == component.id and "runtime" in item.lifecycle)
        assert definition.artifact_id is not None
        artifact = registry.artifacts[definition.artifact_id]
        registry = replace(
            registry,
            components={component.id: component},
            component_order=(component.id,),
            capabilities={capability.id: capability},
            artifacts={artifact.id: artifact},
            health_checks={definition.id: definition},
        )
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        prober = Mock(return_value=CapabilityObservation(capability.id, "missing", detail="probe unavailable"))
        result = diagnose(registry, executor=executor, capability_prober=prober)
        self.assertEqual(result.components[0].status.value, "failed")
        self.assertIn("required_runtime_capability_missing", result.components[0].uncertainties)

    def test_unavailable_capability_probe_stays_unknown(self):
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        from realmheart_maintenance.forensics import CapabilityObservation
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        definition = registry.health_checks["check.core.binary.exists"]
        capability = next(item for item in registry.capabilities.values()
                          if item.component_id == component.id and "runtime" in item.lifecycle)
        assert definition.artifact_id is not None
        artifact = registry.artifacts[definition.artifact_id]
        registry = replace(
            registry,
            components={component.id: component},
            component_order=(component.id,),
            capabilities={capability.id: capability},
            artifacts={artifact.id: artifact},
            health_checks={definition.id: definition},
        )
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        prober = Mock(return_value=CapabilityObservation(capability.id, "unknown", detail="probe result was not available"))
        result = diagnose(registry, executor=executor, capability_prober=prober)
        self.assertEqual(result.components[0].status.value, "unknown")
        self.assertIn("runtime_capability_unknown", result.components[0].uncertainties)

    def _generated_receipt(self, root: Path):
        import os
        from . import _bootstrap
        from realmheart_installer.finalization import build_installed_state_receipt
        from tests.installer.test_verification_engine import Phase13VerificationTests

        helper = Phase13VerificationTests()
        helper.setUp()
        fixture_root = root / "fixture"
        paths, runner, plan, build = helper._fixture(fixture_root)
        environment = {
            "HOME": str(fixture_root / "home"),
            "XDG_CONFIG_HOME": str(fixture_root / "cfg"),
            "XDG_STATE_HOME": str(fixture_root / "state"),
            "PREFIX": str(fixture_root / "prefix"),
            "LIBEXEC": str(fixture_root / "prefix" / "libexec"),
            "SYSCONF": str(fixture_root / "etc"),
        }
        previous = {key: os.environ.get(key) for key in environment}
        os.environ.update(environment)
        try:
            report = helper._engine(paths, runner, plan, build).run()
            payload = build_installed_state_receipt(plan, report)
        finally:
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        path = root / "installed-state.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return path, payload

    def test_receipt_digest_mismatch_is_not_endorsement(self):
        with tempfile.TemporaryDirectory() as temp:
            receipt_path, payload = self._generated_receipt(Path(temp))
            payload["manifest_set_sha256"] = "0" * 64
            receipt_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            from realmheart_maintenance.forensics import load_installed_receipt
            receipt = load_installed_receipt(receipt_path)
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        definition = registry.health_checks["check.core.binary.exists"]
        assert definition.artifact_id is not None
        artifact = registry.artifacts[definition.artifact_id]
        registry = replace(registry, components={component.id: component}, component_order=(component.id,),
                           capabilities={}, artifacts={artifact.id: artifact},
                           health_checks={definition.id: definition})
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        result = diagnose(registry, executor=executor, receipt=receipt)
        self.assertFalse(result.receipt["digest_matches"])
        self.assertNotEqual(result.overall.value, "healthy")

    def test_receipt_alignment_endorses_current_diagnosis(self):
        from unittest.mock import Mock
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_maintenance.forensics import load_installed_receipt
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        with tempfile.TemporaryDirectory() as temp:
            receipt_path, _ = self._generated_receipt(Path(temp))
            receipt = load_installed_receipt(receipt_path)
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        definition = registry.health_checks["check.core.binary.exists"]
        assert definition.artifact_id is not None
        artifact = registry.artifacts[definition.artifact_id]
        registry = replace(registry, components={component.id: component}, component_order=(component.id,),
                           capabilities={}, artifacts={artifact.id: artifact},
                           health_checks={definition.id: definition})
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        result = diagnose(registry, executor=executor, receipt=receipt)
        self.assertTrue(result.receipt["digest_matches"])
        self.assertTrue(result.receipt["transaction_id"])
        self.assertEqual(result.components[0].status.value, "healthy")

    def test_receipt_failed_component_is_not_silently_healthy(self):
        from unittest.mock import Mock
        from dataclasses import replace as _replace
        from realmheart_maintenance.manifest import load_manifest
        from realmheart_maintenance.forensics import load_installed_receipt
        from realmheart_doctor.diagnosis import diagnose
        from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus
        with tempfile.TemporaryDirectory() as temp:
            receipt_path, _ = self._generated_receipt(Path(temp))
            receipt = load_installed_receipt(receipt_path)
        registry = load_manifest(Path("components"))
        component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
        definition = registry.health_checks["check.core.binary.exists"]
        assert definition.artifact_id is not None
        artifact = registry.artifacts[definition.artifact_id]
        registry = replace(registry, components={component.id: component}, component_order=(component.id,),
                           capabilities={}, artifacts={artifact.id: artifact},
                           health_checks={definition.id: definition})
        receipt = _replace(receipt, components={
            **receipt.components,
            component.id: _replace(receipt.components[component.id], health="failed"),
        })
        executor = Mock()
        executor.execute.return_value = HealthCheckReport((HealthCheckResult(
            definition.id, component.id, definition.check, HealthStatus.PASS, "observed"),), 0)
        result = diagnose(registry, executor=executor, receipt=receipt)
        self.assertEqual(result.components[0].status.value, "failed")
        self.assertIn("receipt_component_failed", result.components[0].uncertainties)

    def test_empty_check_set_cannot_report_healthy(self):
        with tempfile.TemporaryDirectory() as temp:
            Path(temp, "manifest.toml").write_text('''schema_version = 1
release_version = "0.7.8"
[[components]]
id = "demo"
name = "Demo"
component_version = "0.7.8"
category = "core"
stage = "core_shell"
''')
            code, payload = self.invoke("doctor", "demo", "--manifest-dir", temp, "--json")
        self.assertEqual(code, 3)
        self.assertEqual(payload["overall"], "unknown")
        self.assertEqual(payload["components"][0]["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
