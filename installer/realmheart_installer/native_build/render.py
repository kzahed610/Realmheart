"""Human renderer for Phase-10 build/stage reports."""

from __future__ import annotations

from .models import BuildStageReport


def _bytes(value: int) -> str:
    amount = float(value)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if amount < 1024 or unit == "GiB":
            return f"{amount:.1f} {unit}" if unit != "B" else f"{int(amount)} B"
        amount /= 1024
    return f"{value} B"


def render_build_stage_report(report: BuildStageReport, *, verbose: bool = False) -> str:
    lines = ["Realmheart native build + staged install", ""]
    lines.extend([
        "Build",
        f"  Configure ........... {'PASS' if report.configured else 'FAILED'}",
        f"  Required targets .... {'PASS' if report.required_targets_built else 'FAILED'}",
        f"  Native self-checks .. {'PASS' if report.self_checks_passed else 'FAILED'}",
        f"  DESTDIR install ..... {'PASS' if report.staged_install_completed else 'FAILED'}",
        f"  Live targets ........ {'UNCHANGED' if report.live_targets_unchanged else 'DRIFTED'}",
        f"  Eventd unit unchanged {'YES' if report.eventd_unit_unchanged else 'NO'}",
    ])
    if report.eventd_runtime_signature_unchanged is not None:
        lines.append(f"  Eventd runtime ...... {'UNCHANGED' if report.eventd_runtime_signature_unchanged else 'CHANGED'}")
    lines.extend([
        f"  Build directory ..... {report.build_dir}",
        f"  Stage root .......... {report.stage_dir}",
        f"  Staged payload ...... {_bytes(report.staged_payload_bytes)}",
        "",
        "BuildUnits",
    ])
    for unit in report.build_units:
        lines.append(f"  {'PASS' if unit.ok else 'FAIL'} {unit.build_unit_id:<24} {unit.cmake_target}")
    lines.extend(["", "Required staged artifacts"])
    for artifact in report.artifacts:
        lines.append(f"  {'PASS' if artifact.ok else 'FAIL'} {artifact.artifact_id:<30} {artifact.staged_path}")
        if verbose and artifact.exists:
            lines.append(f"      mode={artifact.mode} size={artifact.size_bytes} sha256={artifact.sha256 or '-'}")
        if artifact.reason:
            lines.append(f"      {artifact.reason}")
    if report.provenance:
        p = report.provenance
        lines.extend([
            "",
            "Build provenance",
            f"  Realmheart .......... {p.realmheart_version}",
            f"  Source revision ..... {p.source_revision or 'archive/unavailable'}{' (dirty)' if p.source_dirty else ''}",
            f"  Manifest ............ {p.manifest_digest}",
            f"  Plan ................ {p.plan_digest}",
            f"  CMake ............... {p.cmake_version or 'unknown'}",
            f"  Ninja ............... {p.ninja_version or 'unknown'}",
            f"  C++ compiler ........ {p.cxx_compiler_version or p.cxx_compiler or 'unknown'}",
            f"  Eventd autostart .... {p.eventd_autostart or 'unknown'}",
            f"  Hyprland ............ {p.hyprland_version or 'unknown'}",
            f"  Hyprland commit ..... {p.hyprland_commit or 'unknown'}",
            f"  Hyprland ABI ........ {p.hyprland_abi_hash or 'unknown'}",
            f"  FX build ID ......... {p.fx_build_id or 'unknown'}",
        ])
    if report.accounted_uncommitted_stage_paths:
        lines.extend(["", "Accounted staged-only payload (not selected for live commit)"])
        for path in report.accounted_uncommitted_stage_paths:
            lines.append(f"  - {path}")
    if report.unexpected_stage_paths:
        lines.extend(["", "UNEXPECTED staged payload"])
        for path in report.unexpected_stage_paths:
            lines.append(f"  - {path}")
    if report.warnings:
        lines.extend(["", "Warnings:"])
        lines.extend(f"  - {item}" for item in report.warnings)
    if report.blockers:
        lines.extend(["", "Build/stage blockers:"])
        lines.extend(f"  - {item}" for item in report.blockers)
    if verbose and report.commands:
        lines.extend(["", "Commands"])
        for item in report.commands:
            lines.append(f"  {'PASS' if item.ok else 'FAIL'} {item.label}: {' '.join(item.argv)}")
            if not item.ok:
                if item.stdout_tail.strip():
                    lines.append("      stdout tail: " + item.stdout_tail.strip().replace("\n", " | "))
                if item.stderr_tail.strip():
                    lines.append("      stderr tail: " + item.stderr_tail.strip().replace("\n", " | "))
    lines.extend(["", f"Build/stage result: {'PASS' if report.ok else 'FAILED'}"])
    if report.ok:
        lines.append("No live Realmheart installation/configuration changes were made; validated payload remains in DESTDIR staging.")
    return "\n".join(lines)
