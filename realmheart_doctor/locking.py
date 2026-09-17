"""Cooperative state locking: boot never waits, manual waits briefly."""
from __future__ import annotations

from contextlib import contextmanager
import errno
import fcntl
import os
import time
from pathlib import Path


@contextmanager
def acquire_state_lock(state_root: Path, *, timeout: float = 0.0):
    """Hold an exclusive advisory lock on ``<state>/.lock``.

    ``timeout=0`` (boot mode) never waits: contention raises ``TimeoutError``
    immediately so callers can defer.  Manual paths may pass a short budget.
    The lock file itself is durable; the advisory lock is not inherited.
    """
    state_root = Path(state_root)
    state_root.mkdir(parents=True, exist_ok=True)
    lock_path = state_root / ".lock"
    descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
    deadline = time.monotonic() + max(timeout, 0.0)
    try:
        while True:
            try:
                import fcntl

                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as exc:
                if exc.errno not in (errno.EACCES, errno.EAGAIN):
                    raise
                if time.monotonic() >= deadline:
                    raise TimeoutError("another Doctor operation is active") from exc
                time.sleep(0.05)
        yield lock_path
    finally:
        try:
            import fcntl

            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)
