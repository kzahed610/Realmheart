"""Stable cross-tool filesystem fingerprinting for Realmheart maintenance state."""
from __future__ import annotations

import hashlib
import math
import os
import stat
import time
from pathlib import Path

_CHUNK_SIZE = 1024 * 1024
MAX_FINGERPRINT_ENTRIES = 100_000
MAX_FINGERPRINT_DEPTH = 64
MAX_FINGERPRINT_BYTES = 128 * 1024 * 1024
MAX_SHA256_BYTES = MAX_FINGERPRINT_BYTES
MAX_FINGERPRINT_SECONDS = 30.0


class FingerprintLimitExceeded(RuntimeError):
    """Raised when bounded fingerprint traversal would exceed its budget."""

    def __init__(self, resource: str, limit: int | float, observed: int | float) -> None:
        self.resource = resource
        self.limit = limit
        self.observed = observed
        super().__init__(
            f"fingerprint {resource} limit exceeded: {observed} > {limit}"
        )


def _feed_field(hasher, name: str, value: str | bytes) -> None:
    raw = value.encode("utf-8", errors="surrogateescape") if isinstance(value, str) else value
    hasher.update(name.encode("ascii"))
    hasher.update(b"\0")
    hasher.update(str(len(raw)).encode("ascii"))
    hasher.update(b":")
    hasher.update(raw)
    hasher.update(b"\0")


def _feed_stat(hasher, st: os.stat_result) -> None:
    _feed_field(hasher, "mode", oct(stat.S_IMODE(st.st_mode)))


def _validate_limit(name: str, value: int, hard_limit: int) -> int:
    if type(value) is not int or value < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    if value > hard_limit:
        raise ValueError(f"{name} exceeds hard limit {hard_limit}")
    return value


def _hash_regular_file(
    hasher,
    path: Path,
    *,
    max_bytes: int,
    used_bytes: int,
    deadline: float | None,
    started_at: float,
    seconds_limit: float,
) -> int:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open(os.fspath(path), flags)
    with os.fdopen(descriptor, "rb", closefd=True) as handle:
        while True:
            if deadline is not None and time.monotonic() > deadline:
                raise FingerprintLimitExceeded(
                    "seconds", seconds_limit, time.monotonic() - started_at
                )
            remaining = max_bytes - used_bytes
            chunk = handle.read(min(_CHUNK_SIZE, remaining + 1))
            if not chunk:
                break
            if len(chunk) > remaining:
                raise FingerprintLimitExceeded("bytes", max_bytes, used_bytes + len(chunk))
            hasher.update(chunk)
            used_bytes += len(chunk)
    return used_bytes


def fingerprint_path(
    path: Path,
    *,
    max_entries: int = MAX_FINGERPRINT_ENTRIES,
    max_depth: int = MAX_FINGERPRINT_DEPTH,
    max_bytes: int = MAX_FINGERPRINT_BYTES,
    max_seconds: int | float = MAX_FINGERPRINT_SECONDS,
) -> str:
    """Fingerprint content/shape/mode without following symlinks.

    ``max_entries`` counts descendants of a directory, ``max_depth`` counts
    directory levels below the root, and ``max_bytes`` bounds all regular-file
    content read during the traversal.  The path-only call remains supported
    while using conservative finite defaults.
    """

    max_entries = _validate_limit("max_entries", max_entries, MAX_FINGERPRINT_ENTRIES)
    max_depth = _validate_limit("max_depth", max_depth, MAX_FINGERPRINT_DEPTH)
    max_bytes = _validate_limit("max_bytes", max_bytes, MAX_FINGERPRINT_BYTES)
    if (
        isinstance(max_seconds, bool)
        or not isinstance(max_seconds, (int, float))
        or not math.isfinite(max_seconds)
        or max_seconds < 0
    ):
        raise ValueError("max_seconds must be a finite non-negative number")
    if max_seconds > MAX_FINGERPRINT_SECONDS:
        raise ValueError(f"max_seconds exceeds hard limit {MAX_FINGERPRINT_SECONDS:g}")
    seconds_limit = float(max_seconds)
    path = Path(path)
    hasher = hashlib.sha256()
    used_bytes = 0
    entries_seen = 0
    started_at = time.monotonic()
    deadline = started_at + seconds_limit

    def check_deadline() -> None:
        now = time.monotonic()
        if now > deadline:
            raise FingerprintLimitExceeded("seconds", seconds_limit, now - started_at)

    check_deadline()
    try:
        root_stat = path.lstat()
    except FileNotFoundError:
        _feed_field(hasher, "root", "missing")
        return hasher.hexdigest()

    if stat.S_ISLNK(root_stat.st_mode):
        _feed_field(hasher, "root-type", "symlink")
        _feed_stat(hasher, root_stat)
        _feed_field(hasher, "target", os.readlink(path))
        return hasher.hexdigest()
    if stat.S_ISREG(root_stat.st_mode):
        _feed_field(hasher, "root-type", "file")
        _feed_stat(hasher, root_stat)
        _hash_regular_file(
            hasher,
            path,
            max_bytes=max_bytes,
            used_bytes=used_bytes,
            deadline=deadline,
            started_at=started_at,
            seconds_limit=seconds_limit,
        )
        return hasher.hexdigest()
    if not stat.S_ISDIR(root_stat.st_mode):
        _feed_field(hasher, "root-type", f"special:{stat.S_IFMT(root_stat.st_mode)}")
        _feed_stat(hasher, root_stat)
        return hasher.hexdigest()

    _feed_field(hasher, "root-type", "directory")
    _feed_stat(hasher, root_stat)

    def walk(directory: Path, relative_prefix: Path, depth: int) -> None:
        nonlocal entries_seen, used_bytes
        check_deadline()
        with os.scandir(directory) as iterator:
            entries = []
            for entry in iterator:
                entries_seen += 1
                if entries_seen > max_entries:
                    raise FingerprintLimitExceeded("entries", max_entries, entries_seen)
                entries.append(entry)
        entries.sort(key=lambda item: os.fsencode(item.name))
        for entry in entries:
            check_deadline()
            relative = relative_prefix / entry.name
            st = entry.stat(follow_symlinks=False)
            _feed_field(hasher, "path", relative.as_posix())
            _feed_stat(hasher, st)
            if stat.S_ISLNK(st.st_mode):
                _feed_field(hasher, "type", "symlink")
                _feed_field(hasher, "target", os.readlink(entry.path))
            elif stat.S_ISREG(st.st_mode):
                _feed_field(hasher, "type", "file")
                used_bytes = _hash_regular_file(
                    hasher,
                    Path(entry.path),
                    max_bytes=max_bytes,
                    used_bytes=used_bytes,
                    deadline=deadline,
                    started_at=started_at,
                    seconds_limit=seconds_limit,
                )
            elif stat.S_ISDIR(st.st_mode):
                _feed_field(hasher, "type", "directory")
                if depth >= max_depth:
                    raise FingerprintLimitExceeded("depth", max_depth, depth + 1)
                walk(Path(entry.path), relative, depth + 1)
            else:
                _feed_field(hasher, "type", f"special:{stat.S_IFMT(st.st_mode)}")

    walk(path, Path(), 0)
    return hasher.hexdigest()


def fingerprint_regular_bytes(content: bytes, mode: int) -> str:
    hasher = hashlib.sha256()
    _feed_field(hasher, "root-type", "file")
    _feed_field(hasher, "mode", oct(stat.S_IMODE(mode)))
    hasher.update(content)
    return hasher.hexdigest()
