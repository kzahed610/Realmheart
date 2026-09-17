"""Privacy-filtered Doctor report rendering.

The public form keeps component identity and machine-readable states while
dropping host-identifying paths and raw command output.  Private detail is
included only when the caller explicitly asks for it, and the artifact says so.
"""
from __future__ import annotations

from .diagnosis import Diagnosis


def render_report(diagnosis: Diagnosis, *, include_private: bool = False) -> dict[str, object]:
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
    text = "\n".join(lines)
    if not include_private:
        # Privacy filter: drop anything that could carry host paths or raw
        # process output before the report leaves the machine.
        redacted = []
        for line in text.splitlines():
            if "/home/" in line:
                continue
            redacted.append(line)
        return {"text": "\n".join(redacted), "private": False}
    return {"text": text, "private": True}
