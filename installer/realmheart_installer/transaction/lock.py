"""Single-user installer advisory lock."""

from __future__ import annotations

import fcntl
import json
import os
import stat
from datetime import datetime, timezone
from pathlib import Path

from ..errors import InstallerError, LockHeldError


class InstallerLock:
    def __init__(self, path: Path, *, transaction_id: str) -> None:
        self.path = Path(path)
        self.transaction_id = transaction_id
        self._handle = None

    def acquire(self) -> "InstallerLock":
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(self.path, flags, 0o600)
        except OSError as exc:
            raise InstallerError(
                f"Cannot safely open Realmheart installer lock: {self.path}",
                code="RH_INSTALLER_LOCK_UNSAFE", stage="startup",
            ) from exc
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise InstallerError(
                    f"Realmheart installer lock is not a regular file: {self.path}",
                    code="RH_INSTALLER_LOCK_UNSAFE", stage="startup",
                )
            os.fchmod(fd, 0o600)
            handle = os.fdopen(fd, "r+", encoding="utf-8", closefd=True)
            fd = -1
            try:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                handle.close()
                raise LockHeldError(str(self.path)) from exc

            metadata = {
                "pid": os.getpid(),
                "transaction_id": self.transaction_id,
                "started_at": datetime.now(timezone.utc).isoformat(),
            }
            handle.seek(0)
            handle.truncate()
            json.dump(metadata, handle, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
            self._handle = handle
            return self
        except Exception:
            if self._handle is None and 'handle' in locals() and not handle.closed:
                handle.close()
            raise
        finally:
            if fd >= 0:
                os.close(fd)

    def release(self) -> None:
        if self._handle is None:
            return
        try:
            fcntl.flock(self._handle.fileno(), fcntl.LOCK_UN)
        finally:
            self._handle.close()
            self._handle = None

    def __enter__(self) -> "InstallerLock":
        return self.acquire()

    def __exit__(self, exc_type, exc, tb) -> None:
        self.release()
