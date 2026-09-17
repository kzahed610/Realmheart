"""Doctor temporal state: snapshots, last-known-good, and durable history.

State is persistence for Doctor diagnosis.  It is versioned, written
atomically, and treats corrupt historical files as evidence to quarantine, not
as a reason to crash diagnosis.  LKG only ever records reliably verified
HEALTHY component states.
"""
from __future__ import annotations

import errno
import json
import os
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from .diagnosis import ComponentHealth, Diagnosis

STATE_FORMAT_VERSION = 1


@dataclass(frozen=True)
class StateRecord:
    recovered: tuple[str, ...]


def _atomic_write_json(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp"
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    descriptor = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    except OSError as exc:
        if exc.errno not in (errno.EINVAL, getattr(errno, "ENOTSUP", errno.EINVAL),
                             getattr(errno, "EOPNOTSUPP", errno.EINVAL)):
            raise
    finally:
        os.close(descriptor)


def _load_json(path: Path) -> tuple[object | None, bool]:
    try:
        import json as _json

        return _json.loads(path.read_text(encoding="utf-8")), False
    except FileNotFoundError:
        return None, False
    except (OSError, ValueError, RecursionError):
        return None, True


def record_diagnosis(root: Path, diagnosis: Diagnosis, *, now: datetime | None = None) -> StateRecord:
    """Persist one diagnosis as current state and update per-component LKG."""

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(root)
    recovered: list[str] = []
    (root / "corrupt").mkdir(parents=True, exist_ok=True)

    previous_payload, corrupt = _load_json(root / "current.json")
    if corrupt:
        recovered.append("current.json")
        stamp = now.strftime("%Y%m%dT%H%M%S%f")
        os.replace(root / "current.json", root / "corrupt" / f"current-{stamp}.json")

    def component_payload(component) -> dict[str, object]:
        return {
            "status": component.status.value,
            "uncertainties": list(component.uncertainties),
            "checks": [item.to_dict() for item in component.checks],
        }

    current = {
        "format_version": STATE_FORMAT_VERSION,
        "captured_at": now.isoformat(),
        "release_version": diagnosis.release_version,
        "manifest_digest": diagnosis.manifest_digest,
        "overall": diagnosis.overall.value,
        "components": {item.id: component_payload(item) for item in diagnosis.components},
    }
    _atomic_write_json(root / "current.json", current)

    for component in diagnosis.components:
        if component.status is not ComponentHealth.HEALTHY:
            continue
        lkg_path = root / "components" / component.id / "last-healthy.json"
        lkg_payload, lkg_corrupt = _load_json(lkg_path)
        if lkg_corrupt:
            recovered.append(f"components/{component.id}/last-healthy.json")
            stamp = now.strftime("%Y%m%dT%H%M%S%f")
            os.replace(lkg_path, root / "corrupt" / f"last-healthy-{component.id}-{stamp}.json")
        lkg = {
            "format_version": STATE_FORMAT_VERSION,
            "captured_at": now.isoformat(),
            "status": component.status.value,
            "release_version": diagnosis.release_version,
            "manifest_digest": diagnosis.manifest_digest,
            "checks": [item.to_dict() for item in component.checks],
        }
        _atomic_write_json(lkg_path, lkg)
        from .incidents import record_component_recovery

        record_component_recovery(root, component.id, now=now)

    return StateRecord(recovered=tuple(recovered))
