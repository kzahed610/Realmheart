"""Plain terminal renderer for Phase-5 preflight output."""

from __future__ import annotations

from .capabilities import CapabilityState
from .preflight import EnvironmentSnapshot


def render_preflight(snapshot: EnvironmentSnapshot) -> str:
    lines: list[str] = ["Realmheart environment preflight", ""]
    lines.extend([
        "Platform",
        f"  Distribution ........ {snapshot.distro.pretty_name}",
        f"  Architecture ........ {snapshot.architecture}",
        f"  Kernel .............. {snapshot.kernel}",
        f"  Package manager ..... {snapshot.package_manager.kind or 'none detected'}",
        "",
        "Session",
        f"  Wayland ............. {'PASS' if snapshot.session.wayland else 'FAIL'}",
        f"  systemd --user ...... {'PASS' if snapshot.session.systemd_user_available else 'FAIL'}",
        f"  Hyprland ............ {snapshot.hyprland.version or snapshot.hyprland.raw_version or 'unavailable'}",
        f"  Compatibility ....... {snapshot.hyprland.compatibility.value.upper()}",
        f"  Displays ............ {len(snapshot.displays)}",
        "",
        "Realmheart",
        f"  Source target ....... {snapshot.installation.source.version_text or 'unknown'}",
        f"  Source revision ..... {snapshot.installation.source.git_commit or 'unavailable'}{(' (dirty)' if snapshot.installation.source.git_dirty else '')}",
        f"  Existing origin ..... {snapshot.installation.origin.value}",
        f"  Installed version ... {snapshot.installation.installed_version_text or 'none/unknown'}",
        f"  Version evidence .... {snapshot.installation.version_evidence.value}",
        f"  Install mode ........ {snapshot.installation.mode.value.upper() if snapshot.installation.mode else 'UNRESOLVED'}",
        "",
        "Manifest",
        f"  Valid ............... {'PASS' if snapshot.manifest.valid else 'FAIL'}",
        f"  Schema .............. {snapshot.manifest.schema_version if snapshot.manifest.schema_version is not None else 'unknown'}",
        f"  Release ............. {snapshot.manifest.release_version or 'unknown'}",
        f"  Components .......... {snapshot.manifest.component_count}",
        f"  Build units ......... {snapshot.manifest.build_unit_count}",
        f"  Digest .............. {snapshot.manifest.digest or 'unavailable'}",
        "",
        "Capabilities",
    ])
    for result in snapshot.capabilities:
        marker = {
            CapabilityState.PASS: "PASS",
            CapabilityState.MISSING: "MISS",
            CapabilityState.FAILED: "FAIL",
            CapabilityState.NOT_APPLICABLE: "N/A",
        }[result.state]
        suffix = f" [{result.component}]" if result.component else ""
        lines.append(f"  {result.capability_id:<31} {marker:<4} {result.display_name}{suffix}")

    lines.extend(["", f"Environment: {snapshot.state.value.upper()}"])
    if snapshot.blockers:
        lines.append("Blockers:")
        lines.extend(f"  - {item}" for item in snapshot.blockers)
    if snapshot.warnings:
        lines.append("Warnings:")
        lines.extend(f"  - {item}" for item in snapshot.warnings)
    return "\n".join(lines)
