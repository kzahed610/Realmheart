"""Bounded, read-only component-log evidence for incidents and reports.

Collection happens once, when an incident is created, and only for the log
sources a component declares.  Everything is capped (line count, line length,
bytes, deadline), every excerpt is sanitized before it is stored, and a missing
tool or unreadable file degrades to no evidence instead of an error.
"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from realmheart_maintenance.manifest import ManifestRegistry, resolve_canonical_artifact_path

from .redaction import sanitize_text

MAX_SOURCES = 4
MAX_LINES = 40
MAX_LINE_CHARS = 400
MAX_BYTES = 8 * 1024
JOURNAL_TIMEOUT_SECONDS = 2.0


def _default_run(argv: tuple[str, ...], timeout: float) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout, check=False)


def _read_tail(path: Path, max_bytes: int) -> str | None:
    try:
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode):
            return None
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError:
        return None
    try:
        with os.fdopen(descriptor, "rb") as stream:
            stream.seek(0, os.SEEK_END)
            size = stream.tell()
            stream.seek(max(0, size - max_bytes))
            return stream.read(max_bytes).decode("utf-8", errors="replace")
    except OSError:
        return None


def _bounded_lines(text: str) -> list[str]:
    lines = [line.strip() for line in text.replace("\r\n", "\n").replace("\r", "\n").splitlines()]
    lines = [line for line in lines if line]
    return [line[:MAX_LINE_CHARS] for line in lines[-MAX_LINES:]]


def _collect_journal(target: str, *, runner) -> dict | None:
    journalctl = shutil.which("journalctl")
    if journalctl is None:
        return None
    argv = (journalctl, "--user", "-u", target, "--no-pager", "-o", "cat", "-n", str(MAX_LINES))
    try:
        completed = runner(argv, JOURNAL_TIMEOUT_SECONDS)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    lines = _bounded_lines(str(completed.stdout or "")[:MAX_BYTES])
    if not lines:
        return None
    return {"kind": "journal", "target": target, "lines": lines}


def _collect_file(target: str, *, reader) -> dict | None:
    resolved = resolve_canonical_artifact_path(target)
    if not resolved:
        return None
    try:
        text = reader(Path(resolved), MAX_BYTES)
    except (OSError, ValueError, RecursionError):
        return None
    if text is None:
        return None
    lines = _bounded_lines(text)
    if not lines:
        return None
    return {"kind": "file", "target": target, "lines": lines}


def collect_log_evidence(
    registry: ManifestRegistry,
    component_id: str,
    *,
    runner=None,
    reader=None,
    now: datetime | None = None,
) -> dict | None:
    """Return sanitized, bounded log evidence for one component, or ``None``."""

    component = registry.components.get(component_id)
    if component is None or not component.log_sources:
        return None
    run = runner or _default_run
    read = reader or _read_tail
    sources: list[dict] = []
    for spec in component.log_sources[:MAX_SOURCES]:
        if spec.kind == "journal":
            entry = _collect_journal(spec.target, runner=run)
        else:
            entry = _collect_file(spec.target, reader=read)
        if entry is None:
            continue
        try:
            entry["lines"] = [sanitize_text(line) for line in entry["lines"]]
        except (OSError, ValueError, RecursionError):
            continue
        entry["sanitized"] = True
        sources.append(entry)
    if not sources:
        return None
    return {
        "collected_at": (now or datetime.now(timezone.utc)).isoformat(),
        "sources": sources,
    }


def log_collector_for(registry: ManifestRegistry, *, runner=None, reader=None):
    """Return a callable suitable for ``record_component_failure``."""

    def collect(component_id: str) -> dict | None:
        try:
            return collect_log_evidence(registry, component_id, runner=runner, reader=reader)
        except Exception:
            # Evidence collection must never break incident creation.
            return None

    return collect
