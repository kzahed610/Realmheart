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
from .changes import changes_since_healthy
from .classification import classify_failure
from .health import HealthCheckResult, HealthStatus

_UNRESOLVED = "unresolved"
_RESOLVED = "resolved"
_REPLAY_BLOCKING_REPAIR_STATUSES = frozenset({"succeeded", "failed"})


@dataclass(frozen=True)
class IncidentEvent:
    incident_id: str


def _upstream_failures(record: dict) -> tuple[str, ...]:
    """Required dependencies that were observed failing for this component."""

    upstream: list[str] = []
    for entry in record.get("uncertainties") or []:
        if isinstance(entry, str) and entry.startswith("upstream_component_failed:"):
            upstream.extend(part for part in entry.split(":", 1)[1].split(",") if part)
    return tuple(sorted(set(upstream)))


def _blocking_capability_failures(record: dict) -> list[dict]:
    """Return exact runtime capability evidence that made the component fail."""

    entries = record.get("capabilities")
    if not isinstance(entries, list):
        return []
    return [
        dict(item)
        for item in entries
        if isinstance(item, dict)
        and item.get("blocking_failure") is True
        and item.get("state") in {"missing", "failed"}
        and isinstance(item.get("capability_id"), str)
    ]


def _incident_fingerprint(
    component_id: str, failed: list[dict], uncertainties: list,
    capability_failures: list[dict] | None = None,
) -> str:
    conditions = sorted({(str(item.get("check_id")), str(item.get("reason_code")))
                         for item in failed})
    capabilities = sorted({
        (str(item.get("capability_id")), str(item.get("state")), str(item.get("version") or ""))
        for item in (capability_failures or [])
    })
    identity = [component_id, conditions, capabilities, sorted(str(item) for item in uncertainties)]
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
    log_collector=None,
    relevant_changes: tuple[dict[str, object], ...] = (),
) -> IncidentEvent | None:
    """Create or extend the open incident for a failed component.

    Returns ``None`` when the component is not currently FAILED (degradation is
    not incident-worthy by itself), so repeated calls never spam history.
    ``log_collector`` (when provided) is consulted only for a newly created
    incident, so declared component logs are captured once, near the failure.
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
    capability_failures = _blocking_capability_failures(record)
    missing_capabilities = tuple(sorted(
        str(item["capability_id"]) for item in capability_failures if item.get("state") == "missing"
    ))
    failed_capabilities = tuple(sorted(
        str(item["capability_id"]) for item in capability_failures if item.get("state") == "failed"
    ))
    upstream_failed = _upstream_failures(record)
    fingerprint = _incident_fingerprint(
        component_id, failed, record.get("uncertainties", []), capability_failures
    )
    classification = classify_failure(
        tuple(
            HealthCheckResult(
                check_id=str(item.get("check_id")), component_id=component_id, check=item.get("check"),
                status=HealthStatus(item.get("status") or "unknown"), reason_code=str(item.get("reason_code")),
                detail=item.get("detail"),
            )
            for item in failed
        ),
        missing_capabilities=missing_capabilities,
        failed_capabilities=failed_capabilities,
        failed_upstream=upstream_failed,
    )

    existing, incidents_dir = _find_open_incident(root, component_id, fingerprint)
    timestamp = now.isoformat()
    from .journal import journal

    if existing is not None:
        incident_path = incidents_dir / f"{existing}.json"
        _append_timeline(incident_path, {
            "timestamp": timestamp,
            "event_type": "OBSERVATION_APPENDED",
            "summary": "the same unresolved failure was observed again",
            "details": {"checks": [item.get("check_id") for item in failed]},
        }, now)
        journal(root, "observation_appended", incident=existing, component=component_id)
        return IncidentEvent(existing)

    incident_id = _next_incident_id(incidents_dir, now)
    primary = failed[0] if failed else {}
    symptoms = [f"{item.get('check_id')}: {item.get('reason_code')}" for item in failed]
    if not symptoms and capability_failures:
        symptoms = [
            f"runtime capability {item.get('state')}: {item.get('capability_id')}"
            for item in capability_failures[:3]
        ]
    if not symptoms:
        symptoms = ([f"required dependency failed: {name}" for name in upstream_failed]
                    or [str(entry) for entry in (record.get("uncertainties") or [])][:3])
    capability_detail = next(
        (str(item.get("detail")) for item in capability_failures if item.get("detail")), None
    )
    observed = (failed[0].get("detail") if failed else None) or capability_detail or (
        f"required dependency failed: {', '.join(upstream_failed)}" if upstream_failed
        else "component failure was observed")
    log_evidence = None
    if log_collector is not None:
        try:
            log_evidence = log_collector(component_id)
        except Exception:
            log_evidence = None
    last_known_good = _read_lkg_summary(root, component_id)
    temporal_changes = changes_since_healthy(
        current, last_known_good, current_component=record
    )
    for item in relevant_changes:
        candidate = dict(item)
        if candidate not in temporal_changes:
            temporal_changes.append(candidate)
    incident = {
        "format_version": STATE_FORMAT_VERSION,
        "id": incident_id,
        "component_id": component_id,
        "created_at": timestamp,
        "updated_at": timestamp,
        "health_state": "failed",
        "failure_class": classification.failure_class,
        "confidence": classification.confidence,
        "symptoms": symptoms,
        "expected": "component passes its canonical health checks",
        "observed": observed,
        "last_known_good": last_known_good,
        "relevant_changes": temporal_changes,
        "checks": failed,
        "capabilities": capability_failures,
        "repair_attempts": [],
        "raw_logs": log_evidence,
        "resolution_state": _UNRESOLVED,
        "failure_fingerprint": fingerprint,
        "timeline": [{
            "timestamp": timestamp,
            "event_type": (
                "HEALTH_CHECK_FAILED" if failed else
                "CAPABILITY_FAILED" if capability_failures else
                "COMPONENT_FAILED"
            ),
            "summary": f"component {component_id} was diagnosed as failed",
            "details": {
                "check_id": primary.get("check_id") if failed else None,
                "capability_ids": [item.get("capability_id") for item in capability_failures],
            },
        }],
    }
    from .state import _atomic_write_json

    _atomic_write_json(incidents_dir / f"{incident_id}.json", incident)
    journal(root, "incident_opened", incident=incident_id, component=component_id,
            failure_class=classification.failure_class)
    return IncidentEvent(incident_id)


def _read_lkg_summary(root: Path, component_id: str) -> dict | None:
    payload = _read_payload(root / "components" / component_id / "last-healthy.json")
    if payload is None:
        return None
    return {
        "component_id": payload.get("component_id") or component_id,
        "captured_at": payload.get("captured_at"),
        "release_version": payload.get("release_version"),
        "manifest_digest": payload.get("manifest_digest"),
        "checks": payload.get("checks") if isinstance(payload.get("checks"), list) else [],
        "capabilities": payload.get("capabilities") if isinstance(payload.get("capabilities"), list) else [],
        "build_fingerprints": (
            payload.get("build_fingerprints")
            if isinstance(payload.get("build_fingerprints"), list) else []
        ),
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
        from .journal import journal

        journal(root, "incident_resolved", incident=incident_path.stem, component=component_id)
        result = IncidentEvent(incident_path.stem)
    return result


def _target_incident_payload(
    root: Path, component_id: str, incident_id: str | None,
) -> tuple[str | None, Path, dict | None]:
    """Resolve a specific incident when supplied, otherwise the current open one."""

    incidents_dir = _incidents_dir(root)
    if incident_id is None:
        existing, incidents_dir = _find_open_incident(root, component_id)
        if existing is None:
            return None, incidents_dir, None
        payload = _read_payload(incidents_dir / f"{existing}.json")
        return existing, incidents_dir, payload
    # Incident identifiers are generated internally; reject path-like input even
    # for this low-level API so a future caller cannot escape the incident dir.
    if "/" in incident_id or "\\" in incident_id or not incident_id.startswith("RH-"):
        return None, incidents_dir, None
    payload = _read_payload(incidents_dir / f"{incident_id}.json")
    if payload is None or payload.get("component_id") != component_id:
        return None, incidents_dir, None
    return incident_id, incidents_dir, payload


def recorded_attempt_fingerprints(
    root: Path, component_id: str, *, incident_id: str | None = None,
) -> tuple[str, ...]:
    """Return action fingerprints that should block replay for one failure.

    A declined, unavailable, refused, or skipped action was never actually
    attempted and therefore must not poison a later repair.  When an incident
    id is supplied, deduplication is scoped to that exact failure rather than
    every unresolved incident for the component.
    """

    root = Path(root)
    fingerprints: set[str] = set()
    if incident_id is not None:
        resolved_id, incidents_dir, payload = _target_incident_payload(
            root, component_id, incident_id
        )
        candidates = ((resolved_id, payload),) if resolved_id is not None else ()
    else:
        incidents_dir = _incidents_dir(root)
        candidates = []
        for path in sorted(incidents_dir.glob("RH-*.json")):
            payload = _read_payload(path)
            if (payload is not None and payload.get("component_id") == component_id
                    and payload.get("resolution_state") == _UNRESOLVED):
                candidates.append((path.stem, payload))
    for _, payload in candidates:
        if not isinstance(payload, dict):
            continue
        attempts = payload.get("repair_attempts")
        if not isinstance(attempts, list):
            continue
        for item in attempts:
            if (isinstance(item, dict)
                    and isinstance(item.get("fingerprint"), str)
                    and item.get("status") in _REPLAY_BLOCKING_REPAIR_STATUSES):
                fingerprints.add(item["fingerprint"])
    return tuple(sorted(fingerprints))


def record_repair_attempt(
    root: Path,
    component_id: str,
    executions: tuple[dict[str, object], ...],
    *,
    outcome: str,
    now: datetime | None = None,
    incident_id: str | None = None,
) -> IncidentEvent | None:
    """Record manual repair attempts and their verification on the incident.

    ``incident_id`` binds a stateful repair to the failure it was planned for.
    Without one, the legacy behavior is preserved: use the current unresolved
    incident or create a repair-only incident for a degraded component.
    """

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(root)
    existing, incidents_dir, payload = _target_incident_payload(
        root, component_id, incident_id
    )
    timestamp = now.isoformat()
    if incident_id is not None and existing is None:
        # A caller explicitly bound the repair to an incident.  Do not silently
        # invent another record if that target disappeared or mismatched.
        return None
    if existing is None:
        incident_id = _next_incident_id(incidents_dir, now)
        payload = {
            "format_version": STATE_FORMAT_VERSION,
            "id": incident_id,
            "component_id": component_id,
            "created_at": timestamp,
            "updated_at": timestamp,
            "health_state": "repair",
            "failure_class": "REPAIR_ATTEMPT",
            "confidence": "MEDIUM",
            "symptoms": ["a manual repair attempt was recorded"],
            "expected": "component passes its canonical health checks",
            "observed": "manual repair executed",
            "last_known_good": _read_lkg_summary(root, component_id),
            "relevant_changes": [],
            "checks": [],
            "capabilities": [],
            "repair_attempts": [],
            "resolution_state": _UNRESOLVED,
            "failure_fingerprint": hashlib.sha256(
                f"repair:{component_id}:{timestamp}".encode()
            ).hexdigest(),
            "timeline": [],
        }
    else:
        incident_id = existing
        if payload is None or not isinstance(payload.get("timeline"), list):
            return None
    attempts = payload.get("repair_attempts")
    if not isinstance(attempts, list):
        attempts = []
        payload["repair_attempts"] = attempts
    for execution in executions:
        entry = dict(execution)
        entry.setdefault("timestamp", timestamp)
        attempts.append(entry)
    payload["timeline"].append({
        "timestamp": timestamp,
        "event_type": "REPAIR_ATTEMPTED",
        "summary": f"{len(executions)} repair step(s) recorded; outcome {outcome}",
        "details": {"statuses": [str(item.get("status")) for item in executions]},
    })
    payload["updated_at"] = timestamp
    _atomic_write_json(incidents_dir / f"{incident_id}.json", payload)
    from .journal import journal

    journal(root, "repair_recorded", incident=incident_id, component=component_id, outcome=outcome)
    return IncidentEvent(incident_id)
