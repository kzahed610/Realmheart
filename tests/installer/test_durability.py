from __future__ import annotations

import errno
import unittest
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap  # noqa: F401
from realmheart_installer.durability import fsync_directory


class DirectoryDurabilityTests(unittest.TestCase):
    def test_unsupported_directory_fsync_is_the_only_ignored_class(self) -> None:
        with patch("realmheart_installer.durability.os.open", return_value=17), \
             patch("realmheart_installer.durability.os.close") as close, \
             patch("realmheart_installer.durability.os.fsync", side_effect=OSError(errno.EINVAL, "unsupported")):
            fsync_directory(Path("/synthetic"))
        close.assert_called_once_with(17)

    def test_real_directory_fsync_io_failure_propagates(self) -> None:
        with patch("realmheart_installer.durability.os.open", return_value=19), \
             patch("realmheart_installer.durability.os.close") as close, \
             patch("realmheart_installer.durability.os.fsync", side_effect=OSError(errno.EIO, "I/O failure")):
            with self.assertRaises(OSError) as captured:
                fsync_directory(Path("/synthetic"))
        self.assertEqual(captured.exception.errno, errno.EIO)
        close.assert_called_once_with(19)

    def test_directory_open_failure_propagates(self) -> None:
        with patch("realmheart_installer.durability.os.open", side_effect=OSError(errno.EACCES, "denied")):
            with self.assertRaises(OSError) as captured:
                fsync_directory(Path("/synthetic"))
        self.assertEqual(captured.exception.errno, errno.EACCES)


if __name__ == "__main__":
    unittest.main()
