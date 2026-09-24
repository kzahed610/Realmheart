"""Emergency machine-authored reports for terminal Installer failures."""
from __future__ import annotations

import hashlib
import json
import platform
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path

from realmheart_maintenance.redaction import sanitize_text, scan_secrets

from ..constants import INSTALLER_VERSION
from ..errors import InstallerError


def _safe(value: object | None, *, limit: int = 2000) -> str | None:
    if value is None:
        return None
    text = sanitize_text(str(value)).strip()
    return text[:limit] if text else None


def _safe_frames(exc: BaseException, *, limit: int = 6) -> tuple[str, ...]:
    if exc.__traceback__ is None:
        return ()
    frames = traceback.extract_tb(exc.__traceback__)[-limit:]
    # Basenames + symbol/line are enough to identify an origin without leaking a
    # checkout/home path or source line contents into a public report.
    return tuple(
        sanitize_text(f"{Path(frame.filename).name}:{frame.lineno} in {frame.name}")
        for frame in frames
    )


def build_failure_report(
    exc: BaseException,
    *,
    transaction_id: str,
    operation: str | None,
    snapshot=None,
    plan=None,
    now: datetime | None = None,
) -> tuple[dict[str, object], str, str, str]:
    """Build JSON, local Markdown, GitHub Markdown and issue title.

    All free-form exception text passes through the shared public redactor.  If
    the residual secret scanner still objects, the report degrades to category
    names rather than publishing the suspicious text.
    """
    now = now or datetime.now(timezone.utc)
    structured = isinstance(exc, InstallerError)
    code = exc.code if structured else "RH_UNEXPECTED_FAILURE"
    stage = exc.stage if structured else "unexpected"
    exception_type = type(exc).__name__
    operation = operation or "installer"
    summary = _safe(exc.message if structured else str(exc)) or "installer failure"
    frames = _safe_frames(exc) if not structured else ()

    fingerprint_input = {
        "source": "installer",
        "operation": operation,
        "code": code,
        "stage": stage,
        "exception_type": exception_type,
        "frames": frames[:3],
    }
    digest = hashlib.sha256(
        json.dumps(fingerprint_input, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    transaction_tag = hashlib.sha256(transaction_id.encode("utf-8")).hexdigest()[:4].upper()
    incident_id = f"RH-DIAG-{now.strftime('%Y%m%d-%H%M%S')}-{digest[:8].upper()}-{transaction_tag}"

    environment: dict[str, object] = {
        "architecture": platform.machine() or "unknown",
        "kernel": platform.release() or "unknown",
        "python": platform.python_version(),
    }
    if snapshot is not None:
        distro = getattr(snapshot, "distro", None)
        hyprland = getattr(snapshot, "hyprland", None)
        environment.update({
            "distribution": _safe(getattr(distro, "pretty_name", None)) or _safe(getattr(distro, "id", None)) or "unknown",
            "hyprland": _safe(getattr(hyprland, "raw_version", None)) or _safe(getattr(hyprland, "version", None)) or "unavailable",
            "hyprland_commit": _safe(getattr(hyprland, "commit", None)),
            "hyprland_abi": _safe(getattr(hyprland, "abi_hash", None)),
        })
    else:
        environment["distribution"] = _safe(platform.system()) or "unknown"

    target_version = _safe(getattr(plan, "target_version", None)) if plan is not None else None
    payload: dict[str, object] = {
        "schema_version": 1,
        "kind": "terminal_failure",
        "incident_id": incident_id,
        "incident_fingerprint": f"RH-INS-FPRINT-{digest[:16].upper()}",
        "created_at": now.astimezone(timezone.utc).isoformat(),
        "source": "installer",
        "installer_version": INSTALLER_VERSION,
        "transaction_id": transaction_id,
        "operation": operation,
        "code": code,
        "stage": stage,
        "exception_type": exception_type,
        "summary": summary,
        "origin_frames": list(frames),
        "target_version": target_version,
        "environment": environment,
        "privacy_contract": [
            "free-form failure text is redacted before serialization",
            "traceback source paths and source-code lines are not included",
            "arbitrary process environment and private file contents are not included",
            "the GitHub artifact is machine-authored and intended for public review before submission",
        ],
    }

    def _lines(*, compact: bool) -> list[str]:
        lines = [
            "# Realmheart Installer terminal failure",
            "",
            f"- Incident: `{incident_id}`",
            f"- Fingerprint: `{payload['incident_fingerprint']}`",
            f"- Error: `{code}`",
            f"- Stage: `{stage or 'unknown'}`",
            f"- Operation: `{operation}`",
            f"- Exception: `{exception_type}`",
            f"- Summary: {summary}",
            f"- Installer: `{INSTALLER_VERSION}`",
        ]
        if target_version:
            lines.append(f"- Realmheart target: `{target_version}`")
        lines.extend([
            "",
            "## Environment",
            "",
            f"- Distribution: `{environment.get('distribution', 'unknown')}`",
            f"- Kernel: `{environment.get('kernel', 'unknown')}`",
            f"- Architecture: `{environment.get('architecture', 'unknown')}`",
            f"- Python: `{environment.get('python', 'unknown')}`",
        ])
        if environment.get("hyprland"):
            lines.append(f"- Hyprland: `{environment['hyprland']}`")
        if frames:
            lines.extend(["", "## Sanitized origin", ""])
            lines.extend(f"- `{frame}`" for frame in frames)
        if not compact:
            lines.extend([
                "",
                "## Privacy contract",
                "",
                *(f"- {item}" for item in payload["privacy_contract"]),
            ])
        lines.extend([
            "",
            "---",
            "_Generated automatically by Realmheart diagnostics. Review this report before publishing it._",
        ])
        return lines

    markdown = "\n".join(_lines(compact=False)).rstrip() + "\n"
    github = "\n".join(_lines(compact=True)).rstrip() + "\n"
    findings = tuple(sorted(set(scan_secrets(markdown) + scan_secrets(github))))
    if findings:
        payload["redaction_warnings"] = list(findings)
        safe_summary = "Public failure text withheld because the residual secret scanner requested local review."
        payload["summary"] = safe_summary
        payload["origin_frames"] = []
        markdown = (
            "# Realmheart Installer terminal failure\n\n"
            f"- Incident: `{incident_id}`\n"
            f"- Fingerprint: `{payload['incident_fingerprint']}`\n"
            f"- Error: `{code}`\n"
            f"- Stage: `{stage or 'unknown'}`\n"
            f"- Summary: {safe_summary}\n"
            f"- Review categories: `{', '.join(findings)}`\n"
        )
        github = markdown + "\n_Generated automatically by Realmheart diagnostics._\n"

    title = f"[Installer] {code}: {exception_type}"
    return payload, markdown, github, title
