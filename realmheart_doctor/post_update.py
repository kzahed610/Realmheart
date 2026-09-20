"""Post-update pacman correlation: read-only, bounded, evidence not proof.

The optional pacman hook drops a tiny marker in /run.  This module turns that
marker (or an explicit ``--since``) into a bounded pacman-log window,
correlates relevant package transactions with the components that depend on
them, and records the result.  It never repairs, never escalates privileges,
and never touches the package database.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, replace
from datetime import datetime, timedelta, timezone
from pathlib import Path

from realmheart_maintenance.manifest import ManifestRegistry
from realmheart_maintenance.packages import PACMAN_DEPENDENCY_PROVIDERS
from realmheart_maintenance.version import RELEASE_VERSION

from .packages import correlate_package_changes
from .state import STATE_FORMAT_VERSION, _atomic_write_json

MARKER_PATH = Path("/run/realmheart/post-update.pending")
PACMAN_LOG_PATH = Path("/var/log/pacman.log")
LOG_TAIL_BYTES = 512 * 1024
MAX_WINDOW_DAYS = 30
_STATE_FILE = "post-update.json"


@dataclass(frozen=True)
class PackageUpdateReport:
    window_start: str
    window_end: str
    transactions: tuple[dict, ...]
    affected_components: tuple[str, ...]
    marker_consumed: bool = False

    def to_dict(self) -> dict[str, object]:
        return {
            "format_version": STATE_FORMAT_VERSION,
            "doctor_version": RELEASE_VERSION,
            "window_start": self.window_start,
            "window_end": self.window_end,
            "transactions": [dict(item) for item in self.transactions],
            "affected_components": list(self.affected_components),
            "marker_consumed": self.marker_consumed,
        }


@dataclass(frozen=True)
class PostUpdateOutcome:
    mode: str  # ran | no_update_marker | no_relevant_changes | log_unavailable | deferred_lock
    report: PackageUpdateReport | None = None

    def to_dict(self) -> dict[str, object]:
        payload: dict[str, object] = {"format_version": 1, "mode": self.mode}
        if self.report is not None:
            payload["report"] = self.report.to_dict()
        return payload


def _read_json(path: Path) -> dict | None:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError):
        return None
    return payload if isinstance(payload, dict) else None


def _read_log_tail(path: Path, *, max_bytes: int = LOG_TAIL_BYTES) -> str | None:
    """Read a bounded tail; an unreadable log is never fabricated."""

    try:
        with open(path, "rb") as handle:
            handle.seek(0, 2)
            size = handle.tell()
            handle.seek(max(0, size - max_bytes))
            return handle.read().decode("utf-8", errors="replace")
    except OSError:
        return None


def pending_window_start(
    state_root: Path,
    *,
    marker_path: Path | None = None,
) -> datetime | None:
    """Return the start of the pending package-update window, if any."""

    marker_path = Path(marker_path) if marker_path is not None else MARKER_PATH
    state = _read_json(Path(state_root) / _STATE_FILE)
    consumed = state.get("consumed_marker_mtime") if state is not None else None
    try:
        stat = Path(marker_path).stat()
    except OSError:
        return None
    if isinstance(consumed, (int, float)) and stat.st_mtime <= float(consumed):
        return None
    return datetime.fromtimestamp(stat.st_mtime, tz=timezone.utc)


def relevant_packages(registry: ManifestRegistry) -> dict[str, tuple[str, ...]]:
    """Map package names back to the components that depend on them."""

    mapping: dict[str, set[str]] = {}
    for capability in registry.capabilities.values():
        provider = PACMAN_DEPENDENCY_PROVIDERS.get(capability.dependency_id)
        if provider is None:
            continue
        for package in provider.packages:
            mapping.setdefault(package, set())
            if capability.component_id:
                mapping[package].add(capability.component_id)
    return {package: tuple(sorted(components)) for package, components in sorted(mapping.items())}




def affected_component_closure(
    registry: ManifestRegistry, roots: set[str] | frozenset[str],
) -> tuple[str, ...]:
    """Return directly affected components plus required downstream dependents."""

    affected = set(roots)
    changed = True
    while changed:
        changed = False
        for component in registry.components.values():
            if component.id in affected:
                continue
            if any(dep.required and dep.id in affected for dep in component.realmheart_dependencies):
                affected.add(component.id)
                changed = True
    return tuple(item for item in registry.component_order if item in affected)

def correlate_package_updates(
    registry: ManifestRegistry,
    state_root: Path,
    *,
    since: datetime | None = None,
    marker_path: Path | None = None,
    log_path: Path | None = None,
    now: datetime | None = None,
) -> PackageUpdateReport | None:
    """Correlate the pending update window against the bounded pacman log."""

    if now is None:
        now = datetime.now(timezone.utc)
    marker_path = Path(marker_path) if marker_path is not None else MARKER_PATH
    log_path = Path(log_path) if log_path is not None else PACMAN_LOG_PATH
    window_start = since
    if window_start is None:
        window_start = pending_window_start(Path(state_root), marker_path=marker_path)
        if window_start is None:
            return None
    if window_start.tzinfo is None:
        window_start = window_start.replace(tzinfo=timezone.utc)
    window_start = max(window_start, now - timedelta(days=MAX_WINDOW_DAYS))
    log_text = _read_log_tail(Path(log_path))
    if log_text is None:
        return None
    packages = relevant_packages(registry)
    transactions = correlate_package_changes(
        log_text,
        set(packages),
        window=(int(window_start.timestamp()), int(now.timestamp())),
    )
    directly_affected = {
        component
        for item in transactions
        for component in packages.get(str(item.get("package")), ())
    }
    affected = affected_component_closure(registry, directly_affected)
    return PackageUpdateReport(
        window_start=window_start.isoformat(),
        window_end=now.isoformat(),
        transactions=tuple(transactions),
        affected_components=affected,
    )


def record_package_updates(
    state_root: Path,
    report: PackageUpdateReport,
    *,
    marker_path: Path | None = None,
) -> PackageUpdateReport:
    """Persist the correlation and consume the pacman-hook marker."""

    marker_path = Path(marker_path) if marker_path is not None else MARKER_PATH
    root = Path(state_root)
    payload = report.to_dict()
    try:
        payload["consumed_marker_mtime"] = Path(marker_path).stat().st_mtime
    except OSError:
        pass
    _atomic_write_json(root / _STATE_FILE, payload)
    return replace(report, marker_consumed=True)




def transactions_for_component(
    registry: ManifestRegistry, report: PackageUpdateReport, component_id: str,
) -> tuple[dict[str, object], ...]:
    """Return only package transactions whose dependency closure reaches a component."""

    package_map = relevant_packages(registry)
    selected: list[dict[str, object]] = []
    for transaction in report.transactions:
        package = str(transaction.get("package") or "")
        roots = set(package_map.get(package, ()))
        if component_id in affected_component_closure(registry, roots):
            selected.append(dict(transaction))
    return tuple(selected)

def _run_post_update_locked(
    registry: ManifestRegistry,
    state_root: Path,
    *,
    executor=None,
    notifier=None,
    now: datetime | None = None,
    since: datetime | None = None,
    marker_path: Path | None = None,
    log_path: Path | None = None,
) -> PostUpdateOutcome:
    """Correlate, then re-check only when something relevant actually changed."""

    if now is None:
        now = datetime.now(timezone.utc)
    root = Path(state_root)
    report = correlate_package_updates(
        registry, root, since=since, marker_path=marker_path, log_path=log_path, now=now,
    )
    if report is None:
        if since is None and pending_window_start(root, marker_path=marker_path) is None:
            return PostUpdateOutcome("no_update_marker")
        return PostUpdateOutcome("log_unavailable")
    if not report.transactions or not report.affected_components:
        report = record_package_updates(root, report, marker_path=marker_path)
        from .retention import apply_retention

        apply_retention(root, now_prefix=now.strftime("%Y%m%dT%H%M%S%f"))
        return PostUpdateOutcome("no_relevant_changes", report)
    from .diagnosis import diagnose
    from .incidents import record_component_failure
    from .notify import dispatch_notifications
    from .state import record_diagnosis

    diagnosis = diagnose(
        registry, component_ids=report.affected_components, executor=executor,
        health_context="doctor_background", max_cost="cheap",
    )
    record_diagnosis(root, diagnosis, now=now)
    from .log_evidence import log_collector_for

    collector = log_collector_for(registry)
    for component in diagnosis.components:
        record_component_failure(
            root, component.id, now=now, log_collector=collector,
            relevant_changes=transactions_for_component(registry, report, component.id),
        )
    if notifier is not None:
        dispatch_notifications(root, notifier, now=now)
    report = record_package_updates(root, report, marker_path=marker_path)
    from .retention import apply_retention

    apply_retention(root, now_prefix=now.strftime("%Y%m%dT%H%M%S%f"))
    return PostUpdateOutcome("ran", report)


def run_post_update(
    registry: ManifestRegistry,
    state_root: Path,
    *,
    executor=None,
    notifier=None,
    now: datetime | None = None,
    since: datetime | None = None,
    marker_path: Path | None = None,
    log_path: Path | None = None,
    lock_timeout: float = 0.0,
) -> PostUpdateOutcome:
    """Serialize package correlation and every state mutation as one transaction.

    Automatic post-update runs never wait by default.  When another Doctor
    writer owns the state lock the pending marker remains unconsumed, so a
    later run can retry the same update window safely.
    """

    root = Path(state_root)
    marker = Path(marker_path) if marker_path is not None else MARKER_PATH
    if since is None and pending_window_start(root, marker_path=marker) is None:
        return PostUpdateOutcome("no_update_marker")
    from .locking import acquire_state_lock

    try:
        with acquire_state_lock(root, timeout=lock_timeout):
            return _run_post_update_locked(
                registry, root, executor=executor, notifier=notifier, now=now,
                since=since, marker_path=marker, log_path=log_path,
            )
    except TimeoutError:
        return PostUpdateOutcome("deferred_lock")
