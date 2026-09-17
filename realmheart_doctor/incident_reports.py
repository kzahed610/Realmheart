"""Local, sanitized Markdown exports of saved incidents; no new probes."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import tempfile

from .reports import Report, filter_report

_ID = re.compile(r"RH-\d{8}-\d{3,}")
_MAX_INCIDENT_BYTES = 2 * 1024 * 1024
_SECTIONS = (
    ("What broke", "component_id"), ("Current state", "health_state"),
    ("Observed failure", "observed"), ("Doctor diagnosis", "failure_class"),
    ("Confidence", "confidence"), ("Expected dependency state", "expected"),
    ("Detected dependency state", "checks"), ("Build-time dependency state", "build"),
    ("Last-known-good dependency state", "last_known_good"),
    ("Changes since last healthy state", "relevant_changes"),
    ("Realmheart changes", "realmheart_changes"), ("Configuration changes", "configuration_changes"),
    ("Repair attempts", "repair_attempts"), ("Repair results", "repair_results"),
    ("Final conclusion", "resolution_state"), ("Realmheart/system metadata", "metadata"),
    ("Relevant sanitized logs", "timeline"),
)


def render_incident(incident: dict) -> Report:
    """Render only evidenced sections, ignoring unrecognized top-level data."""
    lines = ["# Realmheart Doctor incident", ""]
    for title, key in (*_SECTIONS, ("Incident ID", "id")):
        value = incident.get(key)
        if value is None or value == "" or value == [] or value == {}:
            continue
        # Indented literal blocks keep embedded HTML/Markdown non-executable.
        text = value if isinstance(value, str) else json.dumps(value, indent=2, sort_keys=True)
        lines.extend([f"## {title}", "", *("    " + line for line in text.splitlines()), ""])
    return filter_report("\n".join(lines))


def load_incident(root: Path, incident_id: str) -> dict:
    if not _ID.fullmatch(incident_id):
        raise ValueError("invalid_incident_id")
    path = root / "incidents" / f"{incident_id}.json"
    # Reject final symlinks and bound input before parsing.
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        import stat
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("invalid_incident_file")
        data = stream.read(_MAX_INCIDENT_BYTES + 1)
    if len(data) > _MAX_INCIDENT_BYTES:
        raise ValueError("incident_too_large")
    payload = json.loads(data)
    if not isinstance(payload, dict) or payload.get("id") != incident_id or payload.get("format_version") != 1:
        raise ValueError("invalid_incident")
    return payload


def write_report(root: Path, incident_id: str) -> tuple[Path, Report]:
    report = render_incident(load_incident(root, incident_id))
    if not report["export_allowed"]:
        raise ValueError("report_requires_review")
    directory = root / "reports"
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / f"{incident_id}.md"
    descriptor, name = tempfile.mkstemp(dir=directory, prefix=".report-", suffix=".tmp")
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(report["text"])
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination, report
