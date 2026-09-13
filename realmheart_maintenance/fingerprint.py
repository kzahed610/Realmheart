"""Stable cross-tool filesystem fingerprinting for Realmheart maintenance state."""
from __future__ import annotations

import hashlib
import os
import stat
from pathlib import Path

_CHUNK_SIZE = 1024 * 1024


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


def _hash_regular_file(hasher, path: Path) -> None:
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(_CHUNK_SIZE)
            if not chunk:
                break
            hasher.update(chunk)


def fingerprint_path(path: Path) -> str:
    """Fingerprint content/shape/mode without following symlinks."""
    path = Path(path)
    hasher = hashlib.sha256()
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
        _hash_regular_file(hasher, path)
        return hasher.hexdigest()
    if not stat.S_ISDIR(root_stat.st_mode):
        _feed_field(hasher, "root-type", f"special:{stat.S_IFMT(root_stat.st_mode)}")
        _feed_stat(hasher, root_stat)
        return hasher.hexdigest()

    _feed_field(hasher, "root-type", "directory")
    _feed_stat(hasher, root_stat)

    def walk(directory: Path, relative_prefix: Path) -> None:
        with os.scandir(directory) as iterator:
            entries = sorted(iterator, key=lambda item: os.fsencode(item.name))
        for entry in entries:
            relative = relative_prefix / entry.name
            st = entry.stat(follow_symlinks=False)
            _feed_field(hasher, "path", relative.as_posix())
            _feed_stat(hasher, st)
            if stat.S_ISLNK(st.st_mode):
                _feed_field(hasher, "type", "symlink")
                _feed_field(hasher, "target", os.readlink(entry.path))
            elif stat.S_ISREG(st.st_mode):
                _feed_field(hasher, "type", "file")
                _hash_regular_file(hasher, Path(entry.path))
            elif stat.S_ISDIR(st.st_mode):
                _feed_field(hasher, "type", "directory")
                walk(Path(entry.path), relative)
            else:
                _feed_field(hasher, "type", f"special:{stat.S_IFMT(st.st_mode)}")

    walk(path, Path())
    return hasher.hexdigest()


def fingerprint_regular_bytes(content: bytes, mode: int) -> str:
    hasher = hashlib.sha256()
    _feed_field(hasher, "root-type", "file")
    _feed_field(hasher, "mode", oct(stat.S_IMODE(mode)))
    hasher.update(content)
    return hasher.hexdigest()
