"""Human renderer for the authoritative Phase-9 InstallationPlan."""

from __future__ import annotations

from collections import Counter
from pathlib import Path

from .models import (
    ArtifactCommitClass,
    BackupKind,
    ConfigActionKind,
    InstallationPlan,
    PlanState,
    ServiceActionKind,
)


def render_installation_plan(plan: InstallationPlan, *, dry_run: bool = False, verbose: bool = False) -> str:
    title = "Realmheart Installer — DRY RUN" if dry_run else "Realmheart Installer — Installation Plan"
    lines = [title, ""]
    installed = plan.current_version or "none"
    lines.extend([
        "Identity",
        f"  Mode ................ {plan.mode.value.upper()}",
        f"  Installed ........... {installed}",
        f"  Target .............. {plan.target_version}",
        f"  Manifest ............ schema {plan.manifest_schema_version} / {plan.manifest_digest}",
        f"  Plan digest ......... {plan.plan_digest}",
        "",
        "Environment",
        f"  Distribution ........ {plan.environment.distro.pretty_name}",
        f"  Architecture ........ {plan.environment.architecture}",
        f"  Hyprland ............ {plan.fx_plan.hyprland_version or 'unavailable'}",
        f"  Displays ............ {len(plan.environment.displays)}",
        f"  Preflight ........... {plan.environment.state.value.upper()}",
        "",
        "Realmheart FX",
        f"  Required ............ {'YES' if plan.fx_plan.required else 'NO'}",
        f"  Compatibility ....... {plan.fx_plan.compatibility.value.upper()}",
        f"  Build unit .......... {plan.fx_plan.build_unit}",
        f"  Build ID ............ {plan.fx_plan.build_id}",
        f"  Planned action ...... {plan.fx_plan.action}",
    ])
    for display in plan.environment.displays:
        geometry = f"{display.width or '?'}x{display.height or '?'}"
        refresh = f"@{display.refresh_hz:.3f}Hz" if display.refresh_hz is not None else ""
        scale = f" scale={display.scale}" if display.scale is not None else ""
        position = f" pos={display.x},{display.y}" if display.x is not None and display.y is not None else ""
        focused = " focused" if display.focused else ""
        lines.append(f"      {display.name}: {geometry}{refresh}{scale}{position}{focused}")

    if plan.fx_plan.hyprland_abi_hash:
        lines.append(f"  Hyprland ABI ........ {plan.fx_plan.hyprland_abi_hash}")
    if plan.fx_plan.rebuild_on_dependency_change:
        lines.append(f"  Rebuild trigger ..... {', '.join(plan.fx_plan.rebuild_on_dependency_change)} identity/version drift")

    lines.extend(["", "Dependencies"])
    if plan.package_actions:
        lines.append(f"  Package manager ..... {plan.package_plan.manager}")
        for action in plan.package_actions:
            versions = ""
            if action.installed_version or action.repository_version:
                versions = f" ({action.installed_version or 'absent'} -> {action.repository_version or 'repo unknown'})"
            lines.append(f"  {action.action.value.upper():<20} {action.package}{versions}")
    else:
        lines.append("  Package changes ..... none")
    if plan.package_plan.unresolved:
        for item in plan.package_plan.unresolved:
            lines.append(f"  UNRESOLVED .......... {item.capability_id}: {item.reason}")

    lines.extend(["", "Safety backups"])
    for action in plan.backup_actions:
        if action.kind is BackupKind.PRESERVE_EXISTING_BASELINE:
            lines.append(f"  PRESERVE ............ {action.destination}")
        else:
            existing = " [already exists]" if action.already_exists else ""
            lines.append(f"  {action.kind.value:<20} {action.destination}{existing}")
            for target in action.targets:
                status = "exists" if target.exists else "absent"
                shown = _display_path(target.path, plan)
                lines.append(f"      {shown} [{status}]")
                if verbose:
                    lines.append(f"          precondition={target.fingerprint}")

    lines.extend(["", "Configuration integration"])
    for action in plan.config_actions:
        target = _display_path(action.target, plan)
        if action.kind is ConfigActionKind.FULL_TREE_REPLACE:
            preserve = ", ".join(_display_path(item, plan) for item in action.preserve) or "none"
            lines.append(f"  FULL TREE ........... {target}")
            lines.append(f"      preserve: {preserve}")
        elif action.kind is ConfigActionKind.MANAGED_BLOCK:
            lines.append(f"  MANAGED BLOCK ....... {target}")
        elif action.kind is ConfigActionKind.READ_ONLY:
            lines.append(f"  UNTOUCHED ........... {target}")
        elif action.kind is ConfigActionKind.RENDERED_FILE:
            lines.append(f"  RENDER/INSTALL ...... {target}")
        elif action.kind is ConfigActionKind.SHARED_SEED:
            verb = "SEED" if action.will_mutate else "PRESERVE"
            lines.append(f"  {verb:<20} {target}")
        elif action.kind is ConfigActionKind.GENERATED_STATE:
            lines.append(f"  GENERATE ............ {target}")
        else:
            lines.append(f"  INSTALL/UPDATE ...... {target}")
        if verbose and action.kind is not ConfigActionKind.READ_ONLY:
            lines.append(
                f"      rollback={action.reversibility.value} backup={action.backup_policy} precondition={action.precondition_fingerprint}"
            )
            if action.render_strategy:
                lines.append(f"      renderer={action.render_strategy}")
                for key, value in action.render_values:
                    lines.append(f"          {key}={_display_path(value, plan)}")

    mutating = [action for action in plan.config_actions if action.will_mutate]
    captured = sum(bool(action.precondition_fingerprint) for action in mutating)
    lines.extend([
        "",
        "Rollback / precondition coverage",
        "  Hyprland tree ....... EXACT (rename-backed staged swap)",
        "  Kitty managed block  GUARDED (full preimage + compare-before-write)",
        "  Realmheart drop-ins . EXACT/GUARDED by ownership class",
        "  Generated state ..... BEST EFFORT / regenerable",
        "  System packages ..... BEST EFFORT; never blindly removed",
        f"  Fingerprints ........ {captured}/{len(mutating)} mutating config targets captured; revalidate before mutation",
    ])

    lines.extend(["", "Native build + staged payload"])
    lines.append("  Configure/build ..... normal user; live Event Surface autostart disabled")
    lines.append(f"  Build directory ..... {_display_path(plan.build.build_dir, plan)}")
    lines.append(f"  DESTDIR staging ..... {_display_path(plan.build.stage_dir, plan)}")
    lines.append(f"  Prefix .............. {plan.build.install_prefix}")
    lines.append("  Stage ............... unprivileged CMake DESTDIR install before live commit")
    if verbose:
        lines.append("  Configure args:")
        for arg in plan.build.configure_args:
            lines.append(f"      {arg}")
        lines.append("  Build environment: " + ", ".join(f"{k}={v}" for k, v in plan.build.build_environment))
        lines.append("  Install environment: " + ", ".join(f"{k}={v}" for k, v in plan.build.install_environment))
        lines.append("  Source prerequisites: " + ", ".join(plan.build.source_prerequisites))
        lines.append("  Installer-safe checks: " + ", ".join(item.id for item in plan.build.verification))
        lines.append("  Staged-only payload: " + ", ".join(plan.build.allowed_uncommitted_stage_paths))
    for unit in plan.build_units:
        target = unit.cmake_target or "declarative/no CMake target"
        abi = f"; ABI-sensitive: {', '.join(unit.abi_sensitive_dependencies)}" if unit.abi_sensitive_dependencies else ""
        lines.append(f"  [{unit.id}] {target}{abi}")

    commit_counts = Counter(action.commit_class for action in plan.artifact_actions)
    lines.extend(["", "Artifact commit classes"])
    lines.append("  Origin .............. native/CMake artifacts are validated in DESTDIR first; classes below describe live commit privilege")
    for commit_class in ArtifactCommitClass:
        if commit_counts[commit_class]:
            lines.append(f"  {commit_class.value:<21} {commit_counts[commit_class]}")
    if verbose:
        for action in plan.artifact_actions:
            lines.append(f"      {action.artifact_id:<28} -> {_display_path(action.target, plan)}")

    lines.extend(["", "Privileged operations"])
    for action in plan.privileged_actions:
        lines.append(f"  {action.target}")
        lines.append(f"      owner={action.owner}:{action.group} mode={action.mode} rollback={action.reversibility.value}")

    lines.extend(["", "User services"])
    for action in plan.service_actions:
        if action.action is ServiceActionKind.DAEMON_RELOAD:
            lines.append("  systemctl --user daemon-reload")
        else:
            lines.append(f"  {action.action.value:<14} {action.service}")
            if verbose:
                lines.append(f"      {action.reason}")

    lines.extend(["", "Component plan"])
    for component in plan.components:
        status = component.dependency_state.upper()
        lines.append(f"  [{component.order:02d}/{len(plan.components):02d}] {component.name} [{component.category}] — {status}")
        if verbose:
            if component.build_units:
                lines.append("      build: " + ", ".join(component.build_units))
            for reason in component.dependency_reasons:
                lines.append("      " + reason)

    expensive = sum(check.cost == "expensive" for check in plan.health_checks)
    side_effectful = sum(check.side_effects not in {"none", "read_only"} for check in plan.health_checks)
    lines.extend([
        "",
        "Activation",
        f"  Expected ............ {plan.activation.expected_state}",
        f"  Fresh session ....... {'YES if activation cannot be proven' if plan.activation.requires_fresh_session_if_unproven else 'NO'}",
        "",
        "Post-install verification",
        f"  Health checks ....... {len(plan.health_checks)}",
        f"  Expensive ........... {expensive}",
        f"  Side-effectful ...... {side_effectful}",
    ])
    if verbose:
        for check in plan.health_checks:
            lines.append(f"  {check.id:<38} {check.cost}/{check.side_effects} timeout={check.timeout_ms}ms")

    lines.extend([
        "",
        "Disk estimate",
        f"  Backup preimages .... {_format_bytes(plan.disk.backup_bytes)}",
        f"  Config staging ...... {_format_bytes(plan.disk.config_staging_bytes)}",
        f"  Known minimum ....... {_format_bytes(plan.disk.known_minimum_bytes)}",
        "  Complete estimate ... NO (native build/DESTDIR measured in Phase 10)",
    ])
    if plan.disk.free_bytes_at_config_root is not None:
        lines.append(f"  Free at config root . {_format_bytes(plan.disk.free_bytes_at_config_root)}")

    if plan.warnings:
        lines.extend(["", "Warnings:"])
        lines.extend(f"  - {warning}" for warning in plan.warnings)
    if plan.blockers:
        lines.extend(["", "Plan blockers:"])
        lines.extend(f"  - {blocker}" for blocker in plan.blockers)

    lines.extend(["", f"Plan result: {plan.state.value.upper()}"])
    if dry_run:
        lines.extend(["", "No Realmheart installation changes were made."])
    return "\n".join(lines)


def _display_path(value: str, plan: InstallationPlan) -> str:
    replacements = (
        (plan.layout.home, "$HOME"),
        (plan.layout.xdg_config_home, "$XDG_CONFIG_HOME"),
        (plan.layout.xdg_state_home, "$XDG_STATE_HOME"),
    )
    result = value
    for prefix, token in replacements:
        if result == prefix:
            return token
        if result.startswith(prefix.rstrip("/") + "/"):
            return token + result[len(prefix):]
    return result


def _format_bytes(value: int) -> str:
    amount = float(value)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if amount < 1024.0 or unit == "TiB":
            return f"{amount:.1f} {unit}" if unit != "B" else f"{int(amount)} B"
        amount /= 1024.0
    return f"{value} B"
