from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance.manifest import (
    ManifestError,
    VersionCompatibility,
    VersionSpec,
    classify_version,
    load_manifest,
)
from realmheart_installer.environment.capabilities import CapabilityScanner, CapabilityState
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.package_manager.pacman import PACMAN_DEPENDENCY_PROVIDERS


class MinimalRunner:
    def which(self, executable: str) -> str | None:
        return f"/usr/bin/{executable}" if executable == "demo" else None

    def run(self, argv, **kwargs):
        return CommandResult(tuple(str(x) for x in argv), 0, "demo 1.0.0\n")


def write_manifest(root: Path, body: str) -> Path:
    path = root / "components"
    path.mkdir(parents=True)
    (path / "test.toml").write_text(
        'schema_version = 1\nrelease_version = "0.7.8"\n\n' + body,
        encoding="utf-8",
    )
    return path


class ManifestTests(unittest.TestCase):
    def test_real_manifest_loads_and_is_complete(self) -> None:
        registry = load_manifest(_bootstrap.REPO_ROOT / "components")
        self.assertEqual(registry.schema_version, 1)
        self.assertEqual(registry.release_version, "0.7.8")
        self.assertEqual(len(registry.components), 18)
        self.assertEqual(len(registry.dependencies), 63)
        self.assertEqual(len(registry.capabilities), 65)
        self.assertEqual(len(registry.artifacts), 37)
        self.assertEqual(len(registry.health_checks), 58)
        self.assertEqual(len(registry.build_units), 9)
        self.assertEqual(registry.artifacts["terminal.generator"].mode, "0755")
        self.assertEqual(registry.artifacts["auth.helper"].mode, "4755")
        self.assertEqual(registry.capabilities["runtime.hyprctl"].probe.args["version_argv"], ["version"])
        self.assertEqual(registry.capabilities["runtime.hyprctl"].probe.args["version_prefix"], "hyprland")
        self.assertEqual(registry.artifacts["terminal.generated-starship"].path, "$XDG_STATE_HOME/realmheart/theme/starship.toml")
        self.assertEqual(len(registry.digest), 64)
        self.assertEqual(len(registry.component_order), len(registry.components))

    def test_manifest_digest_is_deterministic_and_content_sensitive(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            components = write_manifest(root, '''
[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"
''')
            first = load_manifest(components).digest
            second = load_manifest(components).digest
            self.assertEqual(first, second)
            path = components / "test.toml"
            path.write_text(path.read_text() + "\n# digest change\n", encoding="utf-8")
            self.assertNotEqual(first, load_manifest(components).digest)

    def test_invalid_artifact_mode_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            components = write_manifest(root, """
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
type = "config"
ownership = "user"
mode = "rwxr-xr-x"
""")
            with self.assertRaisesRegex(ManifestError, "invalid mode"):
                load_manifest(components)

    def test_newer_schema_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "components"
            root.mkdir()
            (root / "future.toml").write_text('schema_version = 2\nrelease_version = "0.7.8"\n', encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "newer"):
                load_manifest(root)

    def test_duplicate_component_ids_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            body = '''
[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"
[[components]]
id = "core"
name = "Duplicate"
component_version = "release"
category = "core"
stage = "foundation"
'''
            with self.assertRaisesRegex(ManifestError, "duplicate component"):
                load_manifest(write_manifest(Path(temp), body))

    def test_dependency_cycle_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            body = '''
[[components]]
id = "a"
name = "A"
component_version = "release"
category = "core"
stage = "foundation"
[[components.realmheart_dependencies]]
id = "b"
required = true
[[components]]
id = "b"
name = "B"
component_version = "release"
category = "core"
stage = "foundation"
[[components.realmheart_dependencies]]
id = "a"
required = true
'''
            with self.assertRaisesRegex(ManifestError, "cycle"):
                load_manifest(write_manifest(Path(temp), body))

    def test_health_check_must_reference_declared_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            body = '''
[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"
[[health_checks]]
id = "check.bad"
component_id = "core"
check = "artifact_exists"
artifact_id = "missing"
cost = "cheap"
side_effects = "none"
timeout_ms = 100
contexts = ["doctor_manual"]
'''
            with self.assertRaisesRegex(ManifestError, "unresolved artifact"):
                load_manifest(write_manifest(Path(temp), body))

    def test_health_check_contexts_are_declared_and_known(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            body = '''
[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"
[[health_checks]]
id = "check.bad"
component_id = "core"
check = "artifact_exists"
cost = "cheap"
side_effects = "none"
timeout_ms = 100
'''
            with self.assertRaisesRegex(ManifestError, "contexts must declare"):
                load_manifest(write_manifest(Path(temp), body))

            unknown = body.replace("timeout_ms = 100", 'timeout_ms = 100\ncontexts = ["whenever"]')
            with self.assertRaisesRegex(ManifestError, "contexts must declare"):
                load_manifest(write_manifest(Path(temp) / "unknown", unknown))

            declared = body.replace(
                "timeout_ms = 100", 'timeout_ms = 100\ncontexts = ["doctor_manual", "doctor_background"]'
            )
            registry = load_manifest(write_manifest(Path(temp) / "declared", declared))
            self.assertEqual(registry.health_checks["check.bad"].contexts, ("doctor_manual", "doctor_background"))

    def test_tool_specific_handler_field_is_forbidden(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            body = '''
[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"
install = "installer.handlers.install_core"
'''
            with self.assertRaisesRegex(ManifestError, "tool-specific"):
                load_manifest(write_manifest(Path(temp), body))

    def test_version_semantics_distinguish_tested_from_incompatible(self) -> None:
        spec = VersionSpec(minimum_version="0.55.0", tested_ranges=("0.55.x", "0.56.x"), known_incompatible=("0.55.9",))
        self.assertEqual(classify_version(spec, "0.56.2"), VersionCompatibility.SATISFIED_TESTED)
        self.assertEqual(classify_version(spec, "0.57.0"), VersionCompatibility.SATISFIED_UNTESTED)
        self.assertEqual(classify_version(spec, "0.54.9"), VersionCompatibility.INCOMPATIBLE)
        self.assertEqual(classify_version(spec, "0.55.9"), VersionCompatibility.INCOMPATIBLE)

    def test_capability_scanner_consumes_manifest_probe_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            components = write_manifest(root, '''
[[components]]
id = "core"
name = "Core"
component_version = "release"
category = "core"
stage = "foundation"
[[external_dependencies]]
id = "dep.demo"
name = "Demo"
[[capabilities]]
id = "runtime.demo"
dependency_id = "dep.demo"
display_name = "Demo executable"
requirement = "required"
lifecycle = ["runtime"]
component_id = "core"
[capabilities.probe]
kind = "executable"
executable = "demo"
version_argv = ["--version"]
''')
            registry = load_manifest(components)
            scanner = CapabilityScanner(MinimalRunner(), temp_root=root / "tmp", registry=registry)
            result = scanner.scan_all()
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0].capability_id, "runtime.demo")
            self.assertEqual(result[0].state, CapabilityState.PASS)

    def test_all_canonical_dependencies_have_verified_pacman_policy(self) -> None:
        registry = load_manifest(_bootstrap.REPO_ROOT / "components")
        dependency_ids = {cap.dependency_id for cap in registry.capabilities.values() if cap.requirement != "soft"}
        self.assertEqual(sorted(dependency_ids - set(PACMAN_DEPENDENCY_PROVIDERS)), [])

    def test_doctor_style_consumer_does_not_import_installer(self) -> None:
        code = (
            "import sys; from pathlib import Path; "
            "from realmheart_maintenance import load_manifest; "
            f"r=load_manifest(Path({str(_bootstrap.REPO_ROOT / 'components')!r})); "
            "assert r.components and r.health_checks; "
            "assert not any(k == 'realmheart_installer' or k.startswith('realmheart_installer.') for k in sys.modules)"
        )
        proc = subprocess.run([sys.executable, "-c", code], cwd=_bootstrap.REPO_ROOT, text=True, capture_output=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)


if __name__ == "__main__":
    unittest.main()
