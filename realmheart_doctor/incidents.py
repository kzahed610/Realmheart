"""Doctor incident persistence: deduplicated failure records with timelines.

An incident is created when a component becomes FAILED, not for every harmless
warning.  The same unresolved condition appends observations instead of
spamming new incidents; recovery resolves the incident, and a later relapse
starts a fresh one.
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from .state import STATE_FORMAT_VERSION, _atomic_write_json

_UNRESOLVED = "unresolved"
_RESOLVED = "resolved"


@dataclass(frozen=True)
class IncidentEvent:
    incident_id: str


def _incident_fingerprint(component_id: str, failed: list[dict], uncertainties: list) -> str:
    conditions = sorted({(str(item.get("check_id")), str(item.get("reason_code")))
                         for item in failed})
    identity = [component_id, conditions, sorted(str(item) for item in uncertainties)]
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()


def _incidents_dir(root: Path) -> Path:
    return root / "incidents"


def _read_payload(path: Path) -> dict | None:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError):
        return None
    return payload if isinstance(payload, dict) else None


def _find_open_incident(root: Path, component_id: str,
                        fingerprint: str | None = None) -> tuple[str | None, Path]:
    incidents_dir = _incidents_dir(root)
    if not incidents_dir.is_dir():
        return None, incidents_dir
    for path in sorted(incidents_dir.glob("RH-*.json")):
        payload = _read_payload(path)
        if (
            payload is not None
            and payload.get("component_id") == component_id
            and payload.get("resolution_state") == _UNRESOLVED
            and (fingerprint is None or payload.get("failure_fingerprint") == fingerprint)
        ):
            return str(path.stem), incidents_dir
    return None, incidents_dir


def _next_incident_id(incidents_dir: Path, now: datetime) -> str:
    day = now.strftime("%Y%m%d")
    highest = 0
    if incidents_dir.is_dir():
        for path in incidents_dir.glob(f"RH-{day}-*.json"):
            try:
                highest = max(highest, int(path.stem.rsplit("-", 1)[1]))
            except (ValueError, IndexError):
                continue
    return f"RH-{day}-{highest + 1:03d}"


def _append_timeline(incident_path: Path, event: dict[str, object], now: datetime) -> bool:
    payload = _read_payload(incident_path)
    if payload is None or not isinstance(payload.get("timeline"), list):
        return False
    payload["timeline"].append(event)
    payload["updated_at"] = now.isoformat()
    from .state import _atomic_write_json

    _atomic_write_json(incident_path, payload)
    return True


def record_component_failure(
    root: Path,
    component_id: str,
    *,
    now: datetime | None = None,
) -> IncidentEvent | None:
    """Create or extend the open incident for a failed component.

    Returns ``None`` when the component is not currently FAILED (degradation is
    not incident-worthy by itself), so repeated calls never spam history.
    """

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(root)
    current_path = root / "current.json"
    current = _read_payload(current_path)
    if current is None:
        return None
    components = current.get("components")
    if not isinstance(components, dict):
        return None
    record = components.get(component_id)
    if not isinstance(record, dict) or record.get("status") != "failed":
        return None
    checks = [item for item in record.get("checks", []) if isinstance(item, dict)]
    failed = [item for item in checks if item.get("status") == "fail"]
    fingerprint = _incident_fingerprint(component_id, failed, record.get("uncertainties", []))

    existing, incidents_dir = _find_open_incident(root, component_id, fingerprint)
    timestamp = now.isoformat()
    if existing is not None:
        incident_path = incidents_dir / f"{existing}.json"
        _append_timeline(incident_path, {
            "timestamp": timestamp,
            "event_type": "OBSERVATION_APPENDED",
            "summary": "the same unresolved failure was observed again",
            "details": {"checks": [item.get("check_id") for item in failed]},
        }, now)
        return IncidentEvent(existing)

    incident_id = _next_incident_id(incidents_dir, now)
    primary = failed[0] if failed else {}
    incident = {
        "format_version": STATE_FORMAT_VERSION,
        "id": incident_id,
        "component_id": component_id,
        "created_at": timestamp,
        "updated_at": timestamp,
        "health_state": "failed",
        "failure_class": "COMPONENT_FAILED",
        "confidence": "HIGH",
        "symptoms": [f"{item.get('check_id')}: {item.get('reason_code')}" for item in failed],
        "expected": "component passes its canonical health checks",
        "observed": (failed[0].get("detail") if failed else None) or "health check reported failure",
        "last_known_good": _read_lkg_summary(root, component_id),
        "relevant_changes": [],
        "checks": failed or checks,
        "repair_attempts": [],
        "resolution_state": _UNRESOLVED,
        "failure_fingerprint": fingerprint,
        "timeline": [{
            "timestamp": timestamp,
            "event_type": "HEALTH_CHECK_FAILED",
            "summary": f"component {component_id} was diagnosed as failed",
            "details": {"check_id": primary.get("check_id") if failed else None},
        }],
    }
    from .state import _atomic_write_json

    _atomic_write_json(incidents_dir / f"{incident_id}.json", incident)
    return IncidentEvent(incident_id)


def _read_lkg_summary(root: Path, component_id: str) -> dict | None:
    payload = _read_payload(root / "components" / component_id / "last-healthy.json")
    if payload is None:
        return None
    return {
        "captured_at": payload.get("captured_at"),
        "release_version": payload.get("release_version"),
        "manifest_digest": payload.get("manifest_digest"),
    }


def record_component_recovery(
    root: Path,
    component_id: str,
    *,
    now: datetime | None = None,
) -> IncidentEvent | None:
    """Resolve open incidents only when current evidence verifies recovery."""

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(root)
    current = _read_payload(root / "current.json")
    components = current.get("components") if current else None
    record = components.get(component_id) if isinstance(components, dict) else None
    if not isinstance(record, dict) or record.get("status") != "healthy":
        return None
    result = None
    for incident_path in sorted(_incidents_dir(root).glob("RH-*.json")):
        payload = _read_payload(incident_path)
        if (payload is None or payload.get("component_id") != component_id
                or payload.get("resolution_state") != _UNRESOLVED
                or not isinstance(payload.get("timeline"), list)):
            continue
        payload["timeline"].append({
            "timestamp": now.isoformat(),
            "event_type": "INCIDENT_RESOLVED",
            "summary": "component returned to a reliably verified healthy state",
            "details": {},
        })
        payload["updated_at"] = now.isoformat()
        payload["resolution_state"] = _RESOLVED
        _atomic_write_json(incident_path, payload)
        result = IncidentEvent(incident_path.stem)
    return result
