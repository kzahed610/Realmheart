"""Human-readable Phase-17 uninstall plan/result rendering."""
from __future__ import annotations

from collections import Counter

from .models import UninstallPlan, UninstallResult


def render_uninstall_plan(plan: UninstallPlan, *, dry_run: bool = False, verbose: bool = False) -> str:
    title = "Realmheart Uninstall — DRY RUN" if dry_run else "Realmheart Uninstall"
    lines = [
        title,
        "",
        "Managed installation",
        f"  Installed version ... {plan.installed_version or 'unknown'}",
        f"  Install transaction . {plan.install_transaction_id or 'unknown'}",
        f"  Receipt .............. {plan.receipt_path}",
        f"  Baseline ............. {'VALID' if plan.baseline_valid else ('FOUND/INVALID' if plan.baseline_available else 'NOT FOUND')}",
        "",
        "Configuration",
        "  keep-current ......... leave current Hyprland tree; remove only Realmheart Kitty block/integration files",
        f"  restore-baseline ..... {'available' if plan.baseline_valid else 'unavailable'}",
    ]
    changed = [item for item in plan.comparisons if item.changed]
    lines.append(f"  divergence ........... {len(changed)} baseline target(s) differ")
    if changed:
        totals = Counter(diff.kind.value for item in changed for diff in item.differences)
        detail = ", ".join(f"{key}={value}" for key, value in sorted(totals.items())) or "changed"
        lines.append(f"  compare summary ...... {detail}")
    lines.extend(["", "Managed footprint"])
    action_counts = Counter(item.keep_current_action.value for item in plan.footprint)
    for action, count in sorted(action_counts.items()):
        lines.append(f"  {action:<21} {count}")
    if verbose:
        for item in plan.footprint:
            drift = " [DIVERGED]" if item.diverged else ""
            lines.append(f"    {item.keep_current_action.value:<18} {item.target}{drift}")
    lines.extend(["", "Preserved by default"])
    for path in plan.preserved_paths:
        lines.append(f"  {path}")
    if plan.package_cleanup_candidates:
        lines.extend(["", "Optional dependency cleanup"])
        for item in plan.package_cleanup_candidates:
            suffix = f" ({item.version_after})" if item.version_after else ""
            lines.append(f"  {item.package}{suffix}")
        lines.append("  default: KEEP packages; exact pacman removal requires explicit opt-in")
    if plan.warnings:
        lines.append("")
        lines.append("Warnings")
        lines.extend(f"  - {item}" for item in plan.warnings)
    if plan.blockers:
        lines.append("")
        lines.append("Blockers")
        lines.extend(f"  - {item}" for item in plan.blockers)
    lines.extend(["", f"Plan result: {'READY' if plan.ready else 'BLOCKED'}"])
    if dry_run:
        lines.append("No Realmheart uninstall changes were made.")
    return "\n".join(lines)


def render_uninstall_compare(plan: UninstallPlan) -> str:
    lines = ["Realmheart configuration comparison", ""]
    changed = [item for item in plan.comparisons if item.changed]
    if not changed:
        lines.append("Current configuration matches the recorded pre-Realmheart baseline for all compared targets.")
        return "\n".join(lines)
    for item in changed:
        lines.append(item.target)
        for diff in item.differences:
            lines.append(f"  {diff.kind.value:<12} {diff.relative_path}")
    lines.append("")
    lines.append("File contents are intentionally not dumped by the uninstaller compare view.")
    return "\n".join(lines)


def render_uninstall_result(result: UninstallResult) -> str:
    lines = [
        "Realmheart uninstall result",
        "",
        f"Configuration ..... {result.config_action.value}",
        f"Completed ......... {'YES' if result.completed else 'NO'}",
        f"Rollback .......... {'performed successfully' if result.rolled_back else 'not required' if result.completed else 'FAILED/PARTIAL'}",
    ]
    if result.safety_snapshot:
        lines.append(f"Safety snapshot ... {result.safety_snapshot}")
    if result.receipt_retired_to:
        lines.append(f"Receipt archived .. {result.receipt_retired_to}")
    lines.append(f"Removed paths ..... {len(result.removed_paths)}")
    lines.append(f"Restored paths .... {len(result.restored_paths)}")
    if result.package_cleanup:
        lines.append(f"Packages removed .. {len(result.package_cleanup.removed)}/{len(result.package_cleanup.requested)}")
    if result.warnings:
        lines.append("Warnings")
        lines.extend(f"  - {item}" for item in result.warnings)
    if result.errors:
        lines.append("Errors")
        lines.extend(f"  - {item}" for item in result.errors)
    return "\n".join(lines)
