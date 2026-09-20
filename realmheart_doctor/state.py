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
    resolved_incidents: tuple[str, ...] = ()




def default_state_root(*, environ: dict[str, str] | None = None, home: Path | None = None) -> Path:
    """Return the canonical per-user Doctor state directory.

    Installer-generated services pass this path explicitly, while interactive
    incident inspection can derive it from XDG_STATE_HOME (or the standard
    ~/.local/state fallback).  Relative XDG values are ignored as invalid.
    """

    environment = os.environ if environ is None else environ
    configured = environment.get("XDG_STATE_HOME")
    if configured:
        candidate = Path(configured).expanduser()
        if candidate.is_absolute():
            return candidate / "realmheart" / "doctor"
    base_home = Path.home() if home is None else Path(home)
    return base_home / ".local" / "state" / "realmheart" / "doctor"

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




def _aggregate_component_state(components: dict[str, object]) -> str:
    """Aggregate a merged persisted snapshot using Doctor's health precedence."""

    records = [item for item in components.values() if isinstance(item, dict)]
    if not records:
        return ComponentHealth.UNKNOWN.value
    for item in records:
        if item.get("status") != ComponentHealth.FAILED.value:
            continue
        category = item.get("category")
        # Older state files did not persist category.  Treat an unknown failed
        # category conservatively rather than silently downgrading it.
        if category in {"core", "fx", "essential", None}:
            return ComponentHealth.FAILED.value
    if any(item.get("status") == ComponentHealth.UNKNOWN.value for item in records):
        return ComponentHealth.UNKNOWN.value
    if any(item.get("status") != ComponentHealth.HEALTHY.value for item in records):
        return ComponentHealth.DEGRADED.value
    return ComponentHealth.HEALTHY.value

def record_diagnosis(
    root: Path, diagnosis: Diagnosis, *, now: datetime | None = None,
    resolve_incidents: bool = True,
) -> StateRecord:
    """Persist one diagnosis as current state and update per-component LKG.

    ``resolve_incidents=False`` is used by the repair verifier so the repair
    attempt can be attached to the incident it addressed before recovery closes
    that incident.  Callers that are not inside a repair transaction should use
    the default.
    """

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(root)
    from .journal import journal

    journal(
        root, "state_recorded",
        overall=diagnosis.overall.value,
        components=len(diagnosis.components),
        manifest_digest=diagnosis.manifest_digest,
        release_version=diagnosis.release_version,
        budget_exhausted=diagnosis.budget_exhausted,
    )
    recovered: list[str] = []
    resolved_incidents: list[str] = []
    (root / "corrupt").mkdir(parents=True, exist_ok=True)

    previous_payload, corrupt = _load_json(root / "current.json")
    if corrupt:
        recovered.append("current.json")
        stamp = now.strftime("%Y%m%dT%H%M%S%f")
        os.replace(root / "current.json", root / "corrupt" / f"current-{stamp}.json")

    def component_payload(component) -> dict[str, object]:
        return {
            "status": component.status.value,
            "category": component.category,
            "uncertainties": list(component.uncertainties),
            "checks": [item.to_dict() for item in component.checks],
            "capabilities": [item.to_dict() for item in component.capabilities],
            "build_fingerprints": [item.to_dict() for item in component.build_fingerprints],
        }

    diagnosed_components = {item.id: component_payload(item) for item in diagnosis.components}
    current_components: dict[str, object] = dict(diagnosed_components)
    merged_previous = False
    if (
        not diagnosis.complete_snapshot
        and isinstance(previous_payload, dict)
        and previous_payload.get("release_version") == diagnosis.release_version
        and previous_payload.get("manifest_digest") == diagnosis.manifest_digest
        and isinstance(previous_payload.get("components"), dict)
    ):
        current_components = dict(previous_payload["components"])
        current_components.update(diagnosed_components)
        merged_previous = True
    current = {
        "format_version": STATE_FORMAT_VERSION,
        "captured_at": now.isoformat(),
        "release_version": diagnosis.release_version,
        "manifest_digest": diagnosis.manifest_digest,
        "overall": (
            _aggregate_component_state(current_components)
            if merged_previous else diagnosis.overall.value
        ),
        "components": current_components,
        "last_diagnosis_complete": diagnosis.complete_snapshot,
        "last_diagnosed_components": sorted(diagnosed_components),
    }
    _atomic_write_json(root / "current.json", current)

    history_dir = root / "history"
    history_dir.mkdir(parents=True, exist_ok=True)
    snapshot_payload = {
        "format_version": STATE_FORMAT_VERSION,
        "captured_at": now.isoformat(),
        "overall": current["overall"],
        "release_version": diagnosis.release_version,
        "manifest_digest": diagnosis.manifest_digest,
        "components": current["components"],
    }
    comparison = dict(snapshot_payload)
    comparison.pop("captured_at", None)
    latest_history: tuple[Path, dict] | None = None
    for path in sorted(history_dir.glob("snap-*.json")):
        payload, corrupt = _load_json(path)
        if corrupt or not isinstance(payload, dict):
            stamp = now.strftime("%Y%m%dT%H%M%S%f")
            os.replace(path, root / "corrupt" / f"snap-{stamp}-{path.name}")
            continue
        candidate = dict(payload)
        candidate.pop("last_seen", None)
        candidate.pop("captured_at", None)
        if candidate == comparison:
            latest_history = (path, payload)
    if latest_history is not None:
        path, payload = latest_history
        payload["last_seen"] = now.strftime("%Y%m%dT%H%M%S%f")
        _atomic_write_json(path, payload)
    else:
        _atomic_write_json(
            history_dir / f"snap-{now.strftime('%Y%m%dT%H%M%S%f')}.json", snapshot_payload
        )

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
            "component_id": component.id,
            "captured_at": now.isoformat(),
            "status": component.status.value,
            "release_version": diagnosis.release_version,
            "manifest_digest": diagnosis.manifest_digest,
            "checks": [item.to_dict() for item in component.checks],
            "capabilities": [item.to_dict() for item in component.capabilities],
            "build_fingerprints": [item.to_dict() for item in component.build_fingerprints],
        }
        _atomic_write_json(lkg_path, lkg)
        if resolve_incidents:
            from .incidents import record_component_recovery, unresolved_incident_ids

            open_incidents = unresolved_incident_ids(root, component.id)
            event = record_component_recovery(root, component.id, now=now)
            if event is not None:
                resolved_incidents.extend(open_incidents)

    return StateRecord(
        recovered=tuple(recovered),
        resolved_incidents=tuple(resolved_incidents),
    )
