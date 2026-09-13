from __future__ import annotations

import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_maintenance.manifest import ManifestError, load_manifest
from realmheart_installer.components.bindings import InstallerBindingRegistry, InstallerComponentBinding, default_installer_bindings
from realmheart_installer.components.execution import execute_component_graph
from realmheart_installer.models import ComponentResult, ComponentState


def write_graph(root: Path, *, binding_required: bool = False) -> Path:
    components = root / "components"
    components.mkdir()
    components.joinpath("graph.toml").write_text(f'''schema_version = 1
release_version = "0.7.8"

[[components]]
id = "a"
name = "A"
component_version = "release"
category = "core"
stage = "foundation"
requires_installer_binding = {str(binding_required).lower()}

[[components]]
id = "b"
name = "B"
component_version = "release"
category = "essential"
stage = "services"
[[components.realmheart_dependencies]]
id = "a"
required = true

[[components]]
id = "c"
name = "C"
component_version = "release"
category = "qol"
stage = "services"
''', encoding="utf-8")
    return components


class ComponentExecutionTests(unittest.TestCase):
    def test_failing_component_blocks_dependents_but_not_unrelated_components(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = load_manifest(write_graph(Path(temp), binding_required=True))
            def fail_a(context):
                now = datetime.now(timezone.utc)
                return ComponentResult("a", ComponentState.FAILED, "foundation", started_at=now, finished_at=now, reason="boom")
            bindings = InstallerBindingRegistry((InstallerComponentBinding("a", fail_a),))
            results = {item.component_id: item for item in execute_component_graph(manifest, bindings, object())}
            self.assertEqual(results["a"].state, ComponentState.FAILED)
            self.assertEqual(results["b"].state, ComponentState.BLOCKED)
            self.assertEqual(results["b"].blocked_by, ("a",))
            self.assertEqual(results["c"].state, ComponentState.PASS)

    def test_real_manifest_required_bindings_are_declared_by_installer(self) -> None:
        manifest = load_manifest(_bootstrap.REPO_ROOT / "components")
        default_installer_bindings().validate(manifest)

    def test_missing_required_installer_binding_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = load_manifest(write_graph(Path(temp), binding_required=True))
            with self.assertRaisesRegex(ManifestError, "binding"):
                execute_component_graph(manifest, InstallerBindingRegistry(), object())

    def test_generic_components_need_no_python_handler(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = load_manifest(write_graph(Path(temp), binding_required=False))
            results = execute_component_graph(manifest, InstallerBindingRegistry(), object())
            self.assertTrue(all(item.state is ComponentState.PASS for item in results))


if __name__ == "__main__":
    unittest.main()
