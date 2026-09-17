"""Public manual Doctor contract, separate from install acceptance."""
from __future__ import annotations

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
        result = diagnose(registry, "lockscreen-auth", executor=executor)
        self.assertEqual([item.id for item in result.components],
                         ["realmheart-core", "realmheart-fx", "lockscreen-auth"])
        self.assertEqual(result.overall.value, "unknown")
        self.assertEqual(executor.execute.call_args.kwargs["context"], "doctor_manual")

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
