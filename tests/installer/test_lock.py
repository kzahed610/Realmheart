from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.errors import InstallerError, LockHeldError
from realmheart_installer.transaction.lock import InstallerLock


class LockTests(unittest.TestCase):
    def test_second_invocation_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "realmheart-installer.lock"
            first = InstallerLock(path, transaction_id="RH-FIRST").acquire()
            try:
                metadata = json.loads(path.read_text())
                self.assertEqual(metadata["transaction_id"], "RH-FIRST")
                with self.assertRaises(LockHeldError):
                    InstallerLock(path, transaction_id="RH-SECOND").acquire()
            finally:
                first.release()

            second = InstallerLock(path, transaction_id="RH-SECOND").acquire()
            second.release()

    def test_symlink_lock_is_refused_without_touching_target(self) -> None:
        with tempfile.TemporaryDirectory(dir="/dev/shm") as temp:
            root = Path(temp)
            target = root / "innocent.txt"
            target.write_text("do not truncate\n", encoding="utf-8")
            lock_path = root / "realmheart-installer.lock"
            lock_path.symlink_to(target)
            with self.assertRaises(InstallerError) as captured:
                InstallerLock(lock_path, transaction_id="RH-SYMLINK").acquire()
            self.assertEqual(captured.exception.code, "RH_INSTALLER_LOCK_UNSAFE")
            self.assertEqual(target.read_text(encoding="utf-8"), "do not truncate\n")



if __name__ == "__main__":
    unittest.main()
