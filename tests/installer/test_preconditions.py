from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.errors import PreconditionFailedError
from realmheart_installer.filesystem.compare import fingerprint_path
from realmheart_installer.transaction.preconditions import capture_path_precondition, verify_precondition


class PreconditionTests(unittest.TestCase):
    def test_file_change_is_detected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "kitty.conf"
            path.write_text("before\n")
            precondition = capture_path_precondition(path)
            verify_precondition(path, precondition)
            path.write_text("after\n")
            with self.assertRaises(PreconditionFailedError):
                verify_precondition(path, precondition)

    def test_tree_fingerprint_detects_nested_change(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "hypr"
            (root / "custom").mkdir(parents=True)
            (root / "hyprland.conf").write_text("source=a\n")
            (root / "custom/user.conf").write_text("user=true\n")
            before = fingerprint_path(root)
            (root / "custom/user.conf").write_text("user=false\n")
            self.assertNotEqual(before, fingerprint_path(root))

    def test_symlink_is_hashed_as_link_not_followed_content(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            external = root / "external"
            external.write_text("one")
            link = root / "link"
            os.symlink(external, link)
            before = fingerprint_path(link)
            external.write_text("two")
            self.assertEqual(before, fingerprint_path(link))
            link.unlink()
            os.symlink(root / "different", link)
            self.assertNotEqual(before, fingerprint_path(link))


if __name__ == "__main__":
    unittest.main()
