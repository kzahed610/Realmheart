"""Human, JSON and GitHub-safe Phase-15 diagnostic renderers."""
from __future__ import annotations

import json

from ..models import to_jsonable
from .models import DiagnosticReport


def render_json_report(report: DiagnosticReport) -> str:
    return json.dumps(to_jsonable(report), indent=2, sort_keys=True) + "\n"


def render_markdown_report(report: DiagnosticReport) -> str:
    env = report.environment
    verify = report.verification
    lines = [
        "# Realmheart Installer Diagnostic Report",
        "",
        f"- Incident: `{report.incident_id}`",
        f"- Fingerprint: `{report.incident_fingerprint}`",
        f"- Installer: `{report.installer_version}`",
        f"- Mode: `{report.mode or 'unknown'}`",
        f"- Realmheart: `{report.current_version or 'none'} → {report.target_version or 'unknown'}`",
        f"- Manifest: `{report.manifest_digest or 'unavailable'}`",
        f"- Plan: `{report.plan_digest or 'unavailable'}`",
        "",
        "## Health",
        "",
        f"- Install: `{verify.install_health or 'not-run'}`",
        f"- Activation: `{verify.activation_state or 'not-run'}`",
        f"- Runtime: `{verify.runtime_health or 'not-run'}`",
        f"- Verification checks: `{verify.check_count}`",
        "",
        "## Environment (allowlisted)",
        "",
        f"- Distribution: `{env.distribution}` (`{env.distribution_id}`)",
        f"- Architecture: `{env.architecture}`",
        f"- Kernel: `{env.kernel}`",
        f"- Session: `{env.session_type or 'unknown'}` / Wayland `{env.wayland}`",
        f"- Hyprland: `{env.hyprland_version or 'unavailable'}` / `{env.hyprland_compatibility}`",
        f"- Hyprland commit: `{env.hyprland_commit or 'unavailable'}`",
        f"- Hyprland ABI: `{env.hyprland_abi_hash or 'unavailable'}`",
        "",
        "### Displays",
        "",
    ]
    if env.displays:
        for item in env.displays:
            size = f"{item.width or '?'}x{item.height or '?'}"
            refresh = f"@{item.refresh_hz:.3f}Hz" if item.refresh_hz is not None else ""
            lines.append(f"- `{item.name}`: {size}{refresh}, scale={item.scale}, pos={item.x},{item.y}, focused={item.focused}")
    else:
        lines.append("- none observed")

    lines.extend(["", "## Root failures", ""])
    if report.root_failures:
        for root in report.root_failures:
            lines.extend([
                f"### {root.component_name or 'Installer'} — {root.severity.value.upper()}",
                "",
                f"- Codes: {', '.join(f'`{code}`' for code in root.error_codes)}",
                f"- Summary: {root.summary}",
                f"- Affected components: {', '.join(f'`{item}`' for item in root.affected_components) or 'none'}",
                "",
            ])
    else:
        lines.append("No root failures were identified.")

    lines.extend(["", "## Blocked components", ""])
    if report.blocked_components:
        for item in report.blocked_components:
            lines.append(f"- `{item.component_id}` ({item.component_name}) blocked by {', '.join(f'`{dep}`' for dep in item.blocked_by)}")
    else:
        lines.append("- none")

    lines.extend([
        "",
        "## Rollback / recovery availability",
        "",
        f"- Permanent baseline: `{report.rollback.permanent_baseline_available}`",
        f"- Version snapshots: `{report.rollback.version_snapshot_count}`",
        f"- Recovery candidates: `{report.rollback.recovery_candidate_count}`",
        f"- Current transaction journal: `{report.rollback.transaction_journal_available}`",
        f"- Automatic recovery possible: `{report.rollback.automatic_recovery_possible}`",
        f"- Note: {report.rollback.note}",
    ])
    if report.warnings:
        lines.extend(["", "## Warnings", ""])
        lines.extend(f"- {item}" for item in report.warnings)
    lines.extend(["", "## Privacy", ""])
    lines.extend(f"- {item}" for item in report.privacy_contract)
    return "\n".join(lines).rstrip() + "\n"


def render_github_issue(report: DiagnosticReport) -> str:
    roots = report.root_failures
    title = roots[0].component_name if roots and roots[0].component_name else "Installer diagnostics"
    lines = [
        f"## Realmheart installer incident — {title}",
        "",
        f"**Incident fingerprint:** `{report.incident_fingerprint}`",
        f"**Realmheart target:** `{report.target_version or 'unknown'}`",
        f"**Install mode:** `{report.mode or 'unknown'}`",
        f"**Hyprland:** `{report.environment.hyprland_version or 'unavailable'}` (`{report.environment.hyprland_compatibility}`)",
        "",
        "### Root cause summary",
        "",
    ]
    if roots:
        for root in roots:
            lines.append(f"- **{root.component_name or 'Installer'}** — `{root.error_codes[0]}` — {root.summary}")
    else:
        lines.append("- No root failure identified; report may represent a healthy/warning-only state.")
    if report.blocked_components:
        lines.extend(["", "### Blocked/affected components", ""])
        for item in report.blocked_components:
            lines.append(f"- `{item.component_id}` blocked by {', '.join(f'`{dep}`' for dep in item.blocked_by)}")
    lines.extend([
        "",
        "### Environment",
        "",
        f"- OS: `{report.environment.distribution}`",
        f"- Architecture: `{report.environment.architecture}`",
        f"- Kernel: `{report.environment.kernel}`",
        f"- Hyprland commit: `{report.environment.hyprland_commit or 'unavailable'}`",
        f"- Hyprland ABI: `{report.environment.hyprland_abi_hash or 'unavailable'}`",
        f"- Displays: `{len(report.environment.displays)}`",
        "",
        "### Health",
        "",
        f"- Install: `{report.verification.install_health or 'not-run'}`",
        f"- Activation: `{report.verification.activation_state or 'not-run'}`",
        f"- Runtime: `{report.verification.runtime_health or 'not-run'}`",
        "",
        "_Generated by Realmheart Installer diagnostics. Environment is allowlisted and paths are normalized; private file contents are not embedded._",
    ])
    return "\n".join(lines).rstrip() + "\n"
