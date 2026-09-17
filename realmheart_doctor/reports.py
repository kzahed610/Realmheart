"""Privacy-filtered Doctor report rendering.

The public form keeps component identity and machine-readable states while
dropping host-identifying paths and raw command output.  Private detail is
included only when the caller explicitly asks for it, and the artifact says so.
"""
from __future__ import annotations

from .diagnosis import Diagnosis
from .redaction import sanitize_text, scan_secrets
from typing import TypedDict


class Report(TypedDict):
    text: str
    private: bool
    export_allowed: bool
    warnings: list[str]


def filter_report(text: str, *, include_private: bool = False) -> Report:
    # Private mode is not permission to disclose credentials.
    clean = sanitize_text(text, username="", hostname="", home="") if include_private else sanitize_text(text)
    warnings = list(scan_secrets(clean))
    return {"text": "Report withheld: sensitive content requires local review." if warnings else clean,
            "private": include_private, "export_allowed": not warnings and not include_private,
            "warnings": warnings}


def render_report(diagnosis: Diagnosis, *, include_private: bool = False) -> Report:
    lines = [
        f"Realmheart Doctor {diagnosis.release_version} — overall: {diagnosis.overall.value}",
    ]
    for component in diagnosis.components:
        lines.append(f"  {component.id}: {component.status.value}")
        for check in component.checks:
            detail = "" if check.detail is None else f" — {check.detail}"
            lines.append(f"    {check.check_id}: {check.status.value}{detail}")
        if not component.checks:
            lines.append("    (no canonical checks)")
    if diagnosis.budget_exhausted:
        lines.append("  (run budget exhausted; some checks did not complete)")
    return filter_report("\n".join(lines), include_private=include_private)
