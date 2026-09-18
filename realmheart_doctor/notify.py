"""Notification dispatch: actionable-only, deduplicated, failure-isolated.

A notification is worth sending only for a new unresolved incident.  The same
unresolved condition stays silent on every later run, a failed notification
backend never breaks health logic, and delivery is retried on the next run.
"""
from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from .state import _atomic_write_json


def dispatch_notifications(
    root: Path,
    notifier,
    *,
    now: datetime | None = None,
) -> list[dict[str, object]]:
    """Notify once per open incident, recording dedup state per incident."""

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(root)
    results: list[dict[str, object]] = []
    incidents_dir = root / "incidents"
    if not incidents_dir.is_dir():
        return results
    for path in sorted(incidents_dir.glob("RH-*.json")):
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError, RecursionError):
            continue
        if not isinstance(payload, dict) or payload.get("resolution_state") != "unresolved":
            continue
        incident_id = str(payload.get("id") or path.stem)
        component_id = str(payload.get("component_id") or "unknown")
        failure_class = str(payload.get("failure_class") or "UNKNOWN")
        notified_state = payload.get("last_notified_state")
        if notified_state == "unresolved":
            continue
        title = "Realmheart health regression detected"
        body = (
            f"Component {component_id} is unresolved ({failure_class}).\n"
            f"Incident {incident_id}.\n"
            f"Inspect: realmheart doctor --incident {incident_id}"
        )
        try:
            notifier(title, body)
            notified = True
        except Exception:
            notified = False
        results.append({"incident_id": incident_id, "component_id": component_id,
                        "notified": notified})
        from .journal import journal

        journal(root, "notification", incident=incident_id, component=component_id,
                delivered=notified)
        if notified:
            payload["last_notified_state"] = "unresolved"
            payload["updated_at"] = now.isoformat()
            _atomic_write_json(path, payload)
    return results
