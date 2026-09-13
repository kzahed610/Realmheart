from __future__ import annotations

import re
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.constants import INSTALLER_VERSION
from realmheart_installer.environment.support import HYPRLAND_MINIMUM, HYPRLAND_TESTED_MINOR_LINES
from realmheart_maintenance.manifest import load_manifest


class VersionContractTests(unittest.TestCase):
    def test_runtime_version_is_derived_from_cmake_project_version(self) -> None:
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        main = (root / "src/main.cpp").read_text(encoding="utf-8")
        notification_daemon = (root / "src/services/NotificationDaemon.cpp").read_text(encoding="utf-8")
        self.assertRegex(cmake, r"project\(Realmheart VERSION \d+\.\d+\.\d+")
        self.assertIn('REALMHEART_VERSION="${PROJECT_VERSION}"', cmake)
        self.assertIn('command == "--version"', main)
        self.assertIn('REALMHEART_VERSION', main)
        self.assertNotIn('Realmheart 0.1.0', main)
        self.assertNotIn('"0.1.0"', notification_daemon)
        self.assertIn('REALMHEART_VERSION', notification_daemon)

    def test_installer_provenance_version_tracks_realmheart_release(self) -> None:
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"project\(Realmheart VERSION (\d+\.\d+\.\d+)", cmake)
        self.assertIsNotNone(match)
        self.assertEqual(INSTALLER_VERSION, match.group(1))
        self.assertNotIn("dev", INSTALLER_VERSION.lower())

    def test_hyprland_runtime_policy_matches_canonical_manifest(self) -> None:
        root = Path(__file__).resolve().parents[2]
        registry = load_manifest(root / "components")
        spec = registry.dependencies["dep.runtime.hyprctl"].version
        self.assertEqual(spec.minimum_version, ".".join(str(item) for item in HYPRLAND_MINIMUM))
        tested = tuple(f"{major}.{minor}.x" for major, minor in sorted(HYPRLAND_TESTED_MINOR_LINES))
        self.assertEqual(spec.tested_ranges, tested)



if __name__ == "__main__":
    unittest.main()
