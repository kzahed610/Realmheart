"""Plain package-plan/result renderer."""

from __future__ import annotations

from .base import DependencyPackagePlan, PackageInstallResult


def render_package_plan(plan: DependencyPackagePlan) -> str:
    lines = ["Realmheart dependency package plan", "", f"Package manager: {plan.manager}"]
    if plan.packages:
        lines.append("Packages proposed:")
        lines.extend(f"  - {package}" for package in plan.packages)
    else:
        lines.append("Packages proposed: none")
    if plan.providers:
        lines.extend(["", "Unsatisfied capabilities:"])
        for item in plan.providers:
            packages = ", ".join(item.packages) if item.packages else "no mapping"
            lines.append(f"  {item.capability_id:<31} {item.resolution.value.upper():<11} {packages}")
            lines.append(f"      {item.reason}")
    else:
        lines.extend(["", "All required/component capabilities are already satisfied."])
    if plan.unresolved:
        lines.extend(["", "Unresolved without manual intervention:"])
        lines.extend(f"  - {item.capability_id}: {item.reason}" for item in plan.unresolved)
    if plan.mutation_blockers:
        lines.extend(["", "Package mutation blockers:"])
        lines.extend(f"  - {item}" for item in plan.mutation_blockers)
    return "\n".join(lines)


def render_install_result(result: PackageInstallResult) -> str:
    lines = ["Package transaction", f"  Manager ............. {result.manager}", f"  Exit status ......... {result.returncode}"]
    for item in result.provenance:
        before = item.version_before or "absent"
        after = item.version_after or "absent"
        owner = "installed by Realmheart" if item.installed_by_transaction else "pre-existing/updated"
        lines.append(f"  {item.package:<24} {item.result.upper():<6} {before} -> {after} ({owner})")
    if result.error:
        lines.extend(["", f"Error: {result.error}"])
    return "\n".join(lines)
