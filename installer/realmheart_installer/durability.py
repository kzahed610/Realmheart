"""Small durability helpers shared by installer persistence primitives."""

from __future__ import annotations

import errno
import os
from pathlib import Path


_UNSUPPORTED_DIRECTORY_FSYNC_ERRNOS = {
    errno.EINVAL,
    getattr(errno, "ENOTSUP", errno.EINVAL),
    getattr(errno, "EOPNOTSUPP", errno.EINVAL),
}


def fsync_directory(path: Path) -> None:
    """Synchronize directory metadata after create/remove/rename operations.

    Some filesystems/platforms explicitly report directory fsync as unsupported;
    those errors are the only ones ignored.  Real durability failures such as
    EIO or ENOSPC propagate so a transaction never records success after the
    filesystem told us its metadata could not be made durable.
    """

    descriptor = os.open(Path(path), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        try:
            os.fsync(descriptor)
        except OSError as exc:
            if exc.errno not in _UNSUPPORTED_DIRECTORY_FSYNC_ERRNOS:
                raise
    finally:
        os.close(descriptor)
