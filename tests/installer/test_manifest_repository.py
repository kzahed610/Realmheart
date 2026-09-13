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

    def test_personal_session_leaks_are_not_in_realmheart_defaults(self) -> None:
        text = (_bootstrap.REPO_ROOT / "config/hypr/hyprland/execs.lua").read_text()
        for token in ("Bibata-Modern-Classic", "easyeffects", "gnome-keyring-daemon", "plasma-polkit-agent", "start_geoclue_agent"):
            self.assertNotIn(token, text)
        self.assertIn("dbus-update-activation-environment", text)


if __name__ == "__main__":
    unittest.main()
