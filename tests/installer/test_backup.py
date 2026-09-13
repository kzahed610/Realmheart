from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.errors import BaselineInvalidError
from realmheart_installer.filesystem.backup import ensure_permanent_baseline, validate_backup_snapshot


class BackupTests(unittest.TestCase):
    def test_baseline_is_symlink_safe_and_immutable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            config = root / "config"
            hypr = config / "hypr"
            hypr.mkdir(parents=True)
            (hypr / "hyprland.conf").write_text("original")
            external = root / "private-external"
            external.mkdir()
            (external / "secret").write_text("do not copy")
            os.symlink(external, hypr / "external-link")

            baseline = root / "data/backups/baseline"
            ensure_permanent_baseline(
                baseline,
                {"hypr": hypr, "missing-kitty": config / "kitty/kitty.conf"},
                installer_version="test",
                target_realmheart_version="0.test",
                transaction_id="RH-ONE",
            )
            self.assertTrue(validate_backup_snapshot(baseline).valid)
            copied_link = baseline / "content/hypr/external-link"
            self.assertTrue(copied_link.is_symlink())
            self.assertEqual(os.readlink(copied_link), str(external))
            self.assertFalse((baseline / "content/hypr/external-link/secret").is_file() and not copied_link.is_symlink())

            # A later reinstall validates and reuses the same baseline rather
            # than capturing Realmheart-modified state as a new "original".
            (hypr / "hyprland.conf").write_text("realmheart modified")
            returned = ensure_permanent_baseline(
                baseline,
                {"hypr": hypr},
                installer_version="new",
                target_realmheart_version="0.new",
                transaction_id="RH-TWO",
            )
            self.assertEqual(returned, baseline)
            self.assertEqual((baseline / "content/hypr/hyprland.conf").read_text(), "original")

    def test_corrupt_existing_baseline_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            baseline = root / "baseline"
            baseline.mkdir()
            (baseline / "manifest.json").write_text("{}")
            with self.assertRaises(BaselineInvalidError):
                ensure_permanent_baseline(
                    baseline,
                    {"hypr": root / "hypr"},
                    installer_version="test",
                    target_realmheart_version="test",
                    transaction_id="RH-TEST",
                )
            self.assertEqual((baseline / "manifest.json").read_text(), "{}")


if __name__ == "__main__":
    unittest.main()
