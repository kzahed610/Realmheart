"""Bounded retention for Doctor state; reports are never auto-deleted."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil

_INCIDENT = re.compile(r"RH-\d{8}-\d{3,}\.json$")
_MAX_INCIDENT_BYTES = 2 * 1024 * 1024


def _read_json(path: Path) -> tuple[dict | None, bool]:
    try:
        return json.loads(path.read_text(encoding="utf-8")), False
    except FileNotFoundError:
        return None, False
    except (OSError, ValueError, RecursionError):
        return None, True


def _quarantine(root: Path, path: Path, *, now_prefix: str) -> None:
    quarantine = root / "corrupt"
    quarantine.mkdir(parents=True, exist_ok=True)
    destination = quarantine / f"{path.name}.{now_prefix}"
    counter = 0
    while destination.exists():
        counter += 1
        destination = quarantine / f"{path.name}.{now_prefix}-{counter}"
    shutil.move(str(path), str(destination))


def apply_retention(root: Path, *, resolved_limit: int = 20, now_prefix: str = "retained") -> dict[str, int]:
    """Bound state growth; unresolved incidents and reports are never pruned."""

    root = Path(root)
    summary: dict[str, int] = {"resolved_removed": 0, "compacted": 0, "quarantined": 0}
    incidents_dir = root / "incidents"
    if incidents_dir.is_dir():
        resolved: list[tuple[str, Path]] = []
        for path in sorted(incidents_dir.iterdir()):
            if not _INCIDENT.fullmatch(path.name):
                continue
            payload, corrupt = _read_json(path)
            if corrupt or payload is None:
                if path.stat().st_size > _MAX_INCIDENT_BYTES or corrupt:
                    _quarantine(root, path, now_prefix=now_prefix)
                    summary["quarantined"] += 1
                continue
            state = payload.get("resolution_state")
            day = str(payload.get("id", path.stem))
            if state == "resolved":
                resolved.append((day, path))
        resolved.sort(key=lambda item: item[0])
        excess = len(resolved) - max(resolved_limit, 0)
        for _, path in resolved[:max(0, excess)]:
            path.unlink()
            summary["resolved_removed"] += 1

    history_dir = root / "history"
    if history_dir.is_dir():
        snapshots: list[tuple[str, Path, dict]] = []
        for path in sorted(history_dir.glob("snap-*.json")):
            payload, corrupt = _read_json(path)
            if corrupt or not isinstance(payload, dict):
                _quarantine(root, path, now_prefix=now_prefix)
                summary["quarantined"] += 1
                continue
            snapshots.append((path.name, path, payload))
        # Compaction: identical non-timestamp state keeps the newest file with
        # an updated last_seen marker instead of unbounded growth.
        groups: dict[str, list[tuple[str, Path, dict]]] = {}
        for name, path, payload in snapshots:
            normalized = dict(payload)
            normalized.pop("last_seen", None)
            normalized.pop("captured_at", None)
            key = json.dumps(normalized, sort_keys=True)
            groups.setdefault(key, []).append((name, path, payload))
        for duplicates in groups.values():
            for name, path, _ in duplicates[:-1]:
                path.unlink()
                summary["compacted"] += 1
            if len(duplicates) > 1:
                newest_name, newest_path, payload = duplicates[-1]
                payload["last_seen"] = newest_name[len("snap-"):-len(".json")]
                from .state import _atomic_write_json

                _atomic_write_json(newest_path, payload)
    return summary
