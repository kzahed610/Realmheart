"""Boot Doctor: bounded one-shot health check per compositor session.

Boot mode is read-only, non-interactive, and never privileged.  A session
marker prevents duplicate work; every other state mutation (snapshot, LKG,
incidents, notifications) is reused from the existing modules unchanged.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from .diagnosis import Diagnosis, diagnose
from .incidents import record_component_failure
from .notify import dispatch_notifications
from .state import STATE_FORMAT_VERSION, _atomic_write_json, record_diagnosis


@dataclass(frozen=True)
class BootOutcome:
    mode: str  # "ran" | "already_ran"


def _session_marker_name(session_key: str) -> str:
    digest = hashlib.sha256(session_key.encode("utf-8")).hexdigest()[:24]
    return f"session-{digest}.json"


def run_boot(
    registry,
    state_root: Path,
    *,
    session_key: str,
    executor=None,
    notifier,
    now: datetime | None = None,
    lock_timeout: float = 0.0,
    marker_path: Path | None = None,
    log_path: Path | None = None,
) -> BootOutcome:
    """Run the automatic one-shot health check for this session."""

    from .locking import acquire_state_lock

    if now is None:
        now = datetime.now(timezone.utc)
    state_root = Path(state_root)
    sessions_dir = state_root / "sessions"
    sessions_dir.mkdir(parents=True, exist_ok=True)
    marker = sessions_dir / _session_marker_name(session_key)
    if marker.is_file():
        return BootOutcome("already_ran")
    try:
        with acquire_state_lock(state_root, timeout=lock_timeout):
            return _run_locked(registry, state_root, session_key, marker,
                               executor=executor, notifier=notifier, now=now,
                               marker_path=marker_path, log_path=log_path)
    except TimeoutError:
        return BootOutcome("deferred_lock")


def _run_locked(registry, state_root: Path, session_key: str, marker: Path,
                *, executor, notifier, now: datetime,
                marker_path: Path | None = None, log_path: Path | None = None) -> BootOutcome:
    sessions_dir = state_root / "sessions"
    marker = sessions_dir / _session_marker_name(session_key)
    if marker.is_file():
        return BootOutcome("already_ran")

    diagnosis = diagnose(
        registry,
        executor=executor,
        health_context="doctor_background",
        max_cost="cheap",
    )
    record = record_diagnosis(state_root, diagnosis, now=now)
    for component in diagnosis.components:
        record_component_failure(state_root, component.id, now=now)
    dispatch_notifications(state_root, notifier, now=now)
    from .post_update import correlate_package_updates, record_package_updates

    package_report = None
    try:
        package_report = correlate_package_updates(
            registry, state_root, marker_path=marker_path, log_path=log_path,
        )
    except (OSError, OverflowError, ValueError):
        package_report = None
    if package_report is not None:
        package_report = record_package_updates(state_root, package_report)
    payload = {
        "format_version": STATE_FORMAT_VERSION,
        "session_key_sha": _session_marker_name(session_key),
        "captured_at": now.isoformat(),
        "recovered": list(record.recovered),
        "package_updates": package_report.to_dict() if package_report is not None else None,
    }
    _atomic_write_json(marker, payload)
    return BootOutcome("ran")
