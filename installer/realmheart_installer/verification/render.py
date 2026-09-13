"""Human renderer for Phase-13 observed-state verification."""
from __future__ import annotations

from .models import VerificationCheckState, VerificationReport


def render_verification_report(report: VerificationReport, *, verbose: bool = False) -> str:
    lines = [
        "Realmheart post-install verification",
        "",
        "Health",
        f"  Install .............. {report.install_health.value.upper()}",
        f"  Activation ........... {report.activation.state.value}",
        f"  Runtime .............. {report.activation.runtime_health.value}",
        f"  Components ........... {len(report.components)}",
        f"  Checks ............... {len(report.checks)}",
        f"  Artifact identities .. {sum(1 for item in report.artifacts if item.sha256 or item.immutable_fingerprint)} immutable",
        "",
        "Components",
    ]
    if report.activation.fx_runtime is not None:
        fx = report.activation.fx_runtime
        lines[8:8] = [
            f"  FX runtime ........... {'MATCH' if fx.matches_validated_build is True else 'UNPROVEN' if fx.matches_validated_build is None else 'STALE/NOT LOADED'}",
            f"  FX build ID .......... {fx.build_id or 'unavailable'}",
        ]
    for item in report.components:
        blocked = f" blocked-by={','.join(item.blocked_by)}" if item.blocked_by else ""
        lines.append(f"  {item.state.value.upper():18} {item.display_name}{blocked}")
        if verbose:
            for check in item.checks:
                lines.append(f"      {check.state.value.upper():14} {check.id} — {check.summary}")
    if verbose:
        lines.extend(["", "Observed immutable artifacts"])
        for item in report.artifacts:
            if item.sha256 or item.immutable_fingerprint:
                identity = item.sha256 or item.immutable_fingerprint
                lines.append(f"  {item.artifact_id:32} {item.mode or '-':5} {identity}")
        lines.extend(["", "Observed dependencies"])
        for item in report.dependencies:
            version = f" version={item.version}" if item.version else ""
            lines.append(f"  {item.state.upper():14} {item.capability_id}{version}")
    if report.warnings:
        lines.extend(["", "Warnings:"])
        lines.extend(f"  - {item}" for item in report.warnings)
    if report.blockers:
        lines.extend(["", "Verification blockers:"])
        lines.extend(f"  - {item}" for item in report.blockers)
    lines.extend([
        "",
        "Receipt inputs",
        f"  Schema ............... {report.receipt_inputs.schema_version}",
        f"  Manifest ............. {report.receipt_inputs.manifest_set_sha256}",
        f"  Observed components .. {len(report.receipt_inputs.components)}",
        f"  Observed dependencies  {len(report.receipt_inputs.dependencies)}",
        f"  FX rebuild triggers .. {len(report.receipt_inputs.fx.rebuild_triggers)}",
        "",
        f"Verification result: {'PASS' if report.ok else 'FAILED'}",
    ])
    if report.activation.state.value == "pending_session_restart" and report.ok:
        lines.append("Install-time verification passed; runtime Last Known Good remains unproven until a fresh session.")
    return "\n".join(lines)
