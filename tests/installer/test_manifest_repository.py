from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance.manifest import load_manifest
from realmheart_maintenance.repository import validate_repository


class RepositoryManifestTests(unittest.TestCase):
    def test_repository_matches_canonical_manifest(self) -> None:
        manifest = load_manifest(_bootstrap.REPO_ROOT / "components")
        result = validate_repository(_bootstrap.REPO_ROOT, manifest)
        self.assertTrue(result.ok, "\n".join(result.errors))

    def test_recovered_cliphist_services_are_canonical_sources(self) -> None:
        manifest = load_manifest(_bootstrap.REPO_ROOT / "components")
        text = manifest.artifacts["clipboard.text-service"]
        image = manifest.artifacts["clipboard.image-service"]
        self.assertTrue((_bootstrap.REPO_ROOT / (text.source or "")).is_file())
        self.assertTrue((_bootstrap.REPO_ROOT / (image.source or "")).is_file())
        self.assertIn("@WL_PASTE@", (_bootstrap.REPO_ROOT / text.source).read_text())
        self.assertIn("@CLIPHIST@", (_bootstrap.REPO_ROOT / image.source).read_text())

    def test_cmake_output_name_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "repo"
            shutil.copytree(
                _bootstrap.REPO_ROOT,
                root,
                ignore=shutil.ignore_patterns(".git", "__pycache__", "*.pyc", "build*"),
            )
            artifacts = root / "components/30-artifacts.toml"
            text = artifacts.read_text()
            text = text.replace(
                'path = "$PREFIX/bin/realmheart-power-menu-renderer"',
                'path = "$PREFIX/bin/realmheart_power_menu_renderer"',
            )
            artifacts.write_text(text)
            manifest = load_manifest(root / "components")
            result = validate_repository(root, manifest)
            self.assertFalse(result.ok)
            self.assertTrue(
                any(
                    "core.power-menu-renderer" in error
                    and "realmheart-power-menu-renderer" in error
                    for error in result.errors
                ),
                result.errors,
            )

    def test_journal_log_source_requires_a_matching_service_artifact(self) -> None:
        from dataclasses import replace

        from realmheart_maintenance.manifest import LogSourceSpec

        manifest = load_manifest(_bootstrap.REPO_ROOT / "components")
        core = manifest.components["realmheart-core"]
        doctored = replace(
            manifest,
            components={
                **manifest.components,
                core.id: replace(core, log_sources=(LogSourceSpec("journal", "not-declared.service"),)),
            },
        )
        result = validate_repository(_bootstrap.REPO_ROOT, doctored)
        self.assertFalse(result.ok)
        self.assertTrue(
            any("not-declared.service" in error and "without a matching service artifact" in error
                for error in result.errors),
            result.errors,
        )

    def test_component_without_any_evidence_source_is_rejected(self) -> None:
        from dataclasses import replace

        manifest = load_manifest(_bootstrap.REPO_ROOT / "components")
        capabilities = {key: value for key, value in manifest.capabilities.items()
                        if value.component_id != "screenshot-ocr"}
        health_checks = {key: value for key, value in manifest.health_checks.items()
                         if value.component_id != "screenshot-ocr"}
        doctored = replace(manifest, capabilities=capabilities, health_checks=health_checks)
        result = validate_repository(_bootstrap.REPO_ROOT, doctored)
        self.assertFalse(result.ok)
        self.assertTrue(
            any("screenshot-ocr" in error and "no health check or capability" in error
                for error in result.errors),
            result.errors,
        )

    def test_personal_session_leaks_are_not_in_realmheart_defaults(self) -> None:
        text = (_bootstrap.REPO_ROOT / "config/hypr/hyprland/execs.lua").read_text()
        for token in ("Bibata-Modern-Classic", "easyeffects", "gnome-keyring-daemon", "plasma-polkit-agent", "start_geoclue_agent"):
            self.assertNotIn(token, text)
        self.assertIn("dbus-update-activation-environment", text)


if __name__ == "__main__":
    unittest.main()
