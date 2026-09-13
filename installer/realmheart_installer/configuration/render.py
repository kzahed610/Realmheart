"""Terminal renderer for Phase-11 configuration integration reports."""

from __future__ import annotations

from .models import ConfigurationIntegrationReport


def render_configuration_report(report: ConfigurationIntegrationReport, *, verbose: bool = False) -> str:
    lines = ["Realmheart configuration + terminal integration", "", "Configuration"]
    changed = sum(1 for item in report.mutations if item.changed)
    lines.append(f"  Managed actions ...... {len(report.mutations)}")
    lines.append(f"  Changed .............. {changed}")
    lines.append(f"  Generated artifacts .. {len(report.generated_artifacts)}")
    lines.append(f"  Verification ......... {'PASS' if all(v.ok for v in report.verification) else 'FAILED'}")
    lines.append(f"  Terminal watcher ..... {'PASS' if report.service.ok else 'FAILED'}")
    lines.append(f"  Rolled back .......... {'YES' if report.rolled_back else 'NO'}")

    if verbose:
        lines.extend(["", "Mutations"])
        for item in report.mutations:
            lines.append(f"  {'CHANGED' if item.changed else 'UNCHANGED':9} {item.action_id} -> {item.target}")
            if item.detail:
                lines.append(f"      {item.detail}")
        lines.extend(["", "Generated terminal state"])
        for item in report.generated_artifacts:
            state = "PASS" if item.exists and (item.size or 0) > 0 else "FAIL"
            lines.append(f"  {state:4} {item.artifact_id:32} {item.path}")
        lines.extend(["", "Verification"])
        for item in report.verification:
            lines.append(f"  {'PASS' if item.ok else 'FAIL':4} {item.id:38} {item.detail}")
        if report.service.commands:
            lines.extend(["", "Watcher commands"])
            for command in report.service.commands:
                lines.append("  " + " ".join(command))

    if report.warnings:
        lines.extend(["", "Warnings:"])
        lines.extend(f"  - {item}" for item in report.warnings)
    if report.blockers:
        lines.extend(["", "Configuration blockers:"])
        lines.extend(f"  - {item}" for item in report.blockers)

    lines.extend(["", f"Configuration result: {'PASS' if report.ok else 'FAILED'}"])
    return "\n".join(lines)
