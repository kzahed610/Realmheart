"""Deterministic, symlink-safe path fingerprinting shared by Installer code.

Installer preconditions and backups must be able to fingerprint legitimate
configuration trees larger than the maintenance package's conservative
128-MiB observation default.  Fingerprints remain exact and streaming; the
shared entry-count, depth, and deadline limits still bound traversal.
"""
from pathlib import Path

from realmheart_maintenance.fingerprint import (
    fingerprint_path as _maintenance_fingerprint_path,
    fingerprint_regular_bytes,
)


def fingerprint_path(path: Path) -> str:
    return _maintenance_fingerprint_path(Path(path), max_bytes=None)


__all__ = ["fingerprint_path", "fingerprint_regular_bytes"]
