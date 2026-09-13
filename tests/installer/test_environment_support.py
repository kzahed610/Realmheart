from __future__ import annotations

import unittest

from . import _bootstrap  # noqa: F401
from realmheart_installer.environment.support import (
    HyprlandCompatibility,
    classify_hyprland,
    parse_version,
)


class SupportTests(unittest.TestCase):
    def test_version_parser_accepts_common_hyprland_text(self) -> None:
        self.assertEqual(str(parse_version("Hyprland 0.56.2 built from branch main")), "0.56.2")
        self.assertEqual(str(parse_version("v0.55.0")), "0.55.0")
        self.assertEqual(str(parse_version("0.56")), "0.56.0")

    def test_compatibility_policy(self) -> None:
        self.assertEqual(classify_hyprland(parse_version("0.55.9")), HyprlandCompatibility.INCOMPATIBLE)
        self.assertEqual(classify_hyprland(parse_version("0.56.0")), HyprlandCompatibility.INCOMPATIBLE)
        self.assertEqual(classify_hyprland(parse_version("0.56.1")), HyprlandCompatibility.SUPPORTED)
        self.assertEqual(classify_hyprland(parse_version("0.56.2")), HyprlandCompatibility.PREFERRED)
        self.assertEqual(classify_hyprland(parse_version("0.56.3")), HyprlandCompatibility.SUPPORTED)
        self.assertEqual(classify_hyprland(parse_version("0.57.0")), HyprlandCompatibility.SUPPORTED)
        self.assertEqual(classify_hyprland(parse_version("0.58.0")), HyprlandCompatibility.UNKNOWN)


if __name__ == "__main__":
    unittest.main()
