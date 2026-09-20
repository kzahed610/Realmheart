"""Boot Doctor: bounded one-shot health check per compositor session.

Boot mode is read-only, non-interactive, and never privileged.  A session
marker prevents duplicate work; every other state mutation (snapshot, LKG,
incidents, notifications) is reused from the existing modules unchanged.
"""
from __future__ import annotations

import hashlib
import os
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


def default_session_key() -> str | None:
    """Compositor session signature, else the machine boot id.

    The shell spawn passes the Hyprland signature; a systemd user unit may start
    before the session environment is imported, so it falls back to one marker
    per machine boot instead of refusing to run.
    """

    signature = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
    if signature:
        return signature
    try:
        boot_id = Path("/proc/sys/kernel/random/boot_id").read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return None
    return f"boot-{boot_id}" if boot_id else None


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

    from .post_update import (
        correlate_package_updates, record_package_updates, transactions_for_component,
    )

    # Correlate package drift *before* choosing checks.  A relevant package
    # transaction gives boot a safe narrow starting set; ordinary boots still
    # perform the full cheap snapshot so non-package regressions are observable.
    package_report = None
    try:
        package_report = correlate_package_updates(
            registry, state_root, marker_path=marker_path, log_path=log_path, now=now,
        )
    except (OSError, OverflowError, ValueError):
        package_report = None
    targeted = (
        package_report.affected_components
        if package_report is not None and package_report.transactions
        and package_report.affected_components
        else None
    )
    diagnosis = diagnose(
        registry,
        component_ids=targeted,
        executor=executor,
        health_context="doctor_background",
        max_cost="cheap",
    )
    record = record_diagnosis(state_root, diagnosis, now=now)
    from .log_evidence import log_collector_for

    collector = log_collector_for(registry)
    for component in diagnosis.components:
        record_component_failure(
            state_root, component.id, now=now, log_collector=collector,
            relevant_changes=(
                transactions_for_component(registry, package_report, component.id)
                if package_report is not None else ()
            ),
        )
    if notifier is not None:
        dispatch_notifications(state_root, notifier, now=now)
    if package_report is not None:
        package_report = record_package_updates(
            state_root, package_report, marker_path=marker_path,
        )
    from .retention import apply_retention

    retention = apply_retention(
        state_root, now_prefix=now.strftime("%Y%m%dT%H%M%S%f"),
    )
    payload = {
        "format_version": STATE_FORMAT_VERSION,
        "session_key_sha": _session_marker_name(session_key),
        "captured_at": now.isoformat(),
        "recovered": list(record.recovered),
        "package_updates": package_report.to_dict() if package_report is not None else None,
        "retention": retention,
    }
    _atomic_write_json(marker, payload)
    return BootOutcome("ran")
