"""Independent read-only post-install acceptance assessment.

This intentionally does not import ``realmheart_installer``.  Installer state is
input data, never executable Doctor behavior.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import stat
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping

from realmheart_maintenance.fingerprint import fingerprint_path
from realmheart_maintenance.forensics import (
    ArtifactObservation,
    CapabilityObservation,
    CurrentHealthSnapshot,
    InstalledStateReceipt,
    ReceiptArtifact,
    ReceiptCapability,
    ReceiptComponent,
    analyze_forensics,
)
from realmheart_maintenance.manifest import ManifestRegistry

from .models import AcceptanceAssessment, AcceptanceFinding, AcceptanceRecommendation

CANDIDATE_SCHEMA_VERSION = 1
_SATISFIED = {"pass", "not_applicable"}
_CORE_CATEGORIES = {"core", "fx"}


class DoctorAcceptanceError(ValueError):
    pass


def _mapping(value: Any, field: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise DoctorAcceptanceError(f"{field} must be an object")
    return value


def _text(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise DoctorAcceptanceError(f"{field} must be a non-empty string")
    return value


def load_candidate_bundle(path: Path) -> Mapping[str, Any]:
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise DoctorAcceptanceError(f"candidate bundle must be a regular non-symlink file: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise DoctorAcceptanceError(f"cannot read candidate bundle: {exc}") from exc
    return _mapping(payload, "candidate bundle")


def _candidate_receipt(payload: Mapping[str, Any], registry: ManifestRegistry) -> InstalledStateReceipt:
    if payload.get("schema_version") != CANDIDATE_SCHEMA_VERSION:
        raise DoctorAcceptanceError(f"unsupported candidate schema {payload.get('schema_version')!r}")
    if payload.get("kind") != "realmheart_install_candidate":
        raise DoctorAcceptanceError("candidate bundle kind must be realmheart_install_candidate")
    if payload.get("manifest_set_sha256") != registry.digest:
        raise DoctorAcceptanceError("candidate manifest digest does not match the canonical manifest")
    if payload.get("manifest_schema_version") != registry.schema_version:
        raise DoctorAcceptanceError("candidate manifest schema does not match the canonical manifest")

    components: dict[str, ReceiptComponent] = {}
    for cid, raw_value in _mapping(payload.get("components"), "components").items():
        raw = _mapping(raw_value, f"component {cid}")
        components[cid] = ReceiptComponent(
            component_id=cid,
            display_name=_text(raw.get("display_name"), f"component {cid}.display_name"),
            category=_text(raw.get("category"), f"component {cid}.category"),
            health=_text(raw.get("health"), f"component {cid}.health"),
            blocked_by=tuple(str(x) for x in raw.get("blocked_by", ())),
            artifact_ids=tuple(str(x) for x in raw.get("artifact_ids", ())),
            build_unit_ids=tuple(str(x) for x in raw.get("build_unit_ids", ())),
        )

    capabilities: dict[str, ReceiptCapability] = {}
    for capid, raw_value in _mapping(payload.get("dependencies"), "dependencies").items():
        raw = _mapping(raw_value, f"dependency {capid}")
        capabilities[capid] = ReceiptCapability(
            capability_id=capid,
            component_id=raw.get("component_id") if isinstance(raw.get("component_id"), str) else None,
            requirement=_text(raw.get("requirement"), f"dependency {capid}.requirement"),
            lifecycle=tuple(str(x) for x in raw.get("lifecycle", ())),
            state=_text(raw.get("state"), f"dependency {capid}.state"),
            version=raw.get("version") if isinstance(raw.get("version"), str) else None,
        )

    artifacts: dict[str, ReceiptArtifact] = {}
    for aid, raw_value in _mapping(payload.get("artifacts"), "artifacts").items():
        raw = _mapping(raw_value, f"artifact {aid}")
        artifacts[aid] = ReceiptArtifact(
            artifact_id=aid,
            component_id=_text(raw.get("component_id"), f"artifact {aid}.component_id"),
            path=_text(raw.get("path"), f"artifact {aid}.path"),
            artifact_type=_text(raw.get("type"), f"artifact {aid}.type"),
            ownership=_text(raw.get("ownership"), f"artifact {aid}.ownership"),
            mode=raw.get("mode") if isinstance(raw.get("mode"), str) else None,
            sha256=raw.get("sha256") if isinstance(raw.get("sha256"), str) else None,
            immutable_fingerprint=raw.get("immutable_fingerprint") if isinstance(raw.get("immutable_fingerprint"), str) else None,
        )

    return InstalledStateReceipt(
        schema_version=2,
        realmheart_version=_text(payload.get("realmheart_version"), "realmheart_version"),
        manifest_schema_version=int(payload["manifest_schema_version"]),
        manifest_digest=_text(payload.get("manifest_set_sha256"), "manifest_set_sha256"),
        installer_version=_text(payload.get("installer_version"), "installer_version"),
        transaction_id=_text(payload.get("transaction_id"), "transaction_id"),
        disposition="candidate",
        install_health=_text(payload.get("install_health"), "install_health"),
        activation_state=_text(payload.get("activation_state"), "activation_state"),
        runtime_health=_text(payload.get("runtime_health"), "runtime_health"),
        components=MappingProxyType(components),
        capabilities=MappingProxyType(capabilities),
        artifacts=MappingProxyType(artifacts),
    )


def _run(argv: tuple[str, ...], *, timeout: float = 4.0) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(argv, text=True, capture_output=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None


def _probe_capability(spec) -> CapabilityObservation:
    kind = spec.probe.kind
    args = spec.probe.args
    try:
        if kind == "executable":
            executable = str(args["executable"])
            path = shutil.which(executable)
            if not path:
                return CapabilityObservation(spec.id, "missing", detail=f"{executable} not found")
            version = None
            version_argv = tuple(str(x) for x in args.get("version_argv", ()))
            if version_argv:
                result = _run((path, *version_argv))
                if result is None or result.returncode != 0:
                    return CapabilityObservation(spec.id, "failed", detail=f"cannot query {executable} version")
                combined = (result.stdout or result.stderr).strip()
                version = combined.splitlines()[0] if combined else None
            return CapabilityObservation(spec.id, "pass", version=version, detail=f"{executable} available")

        if kind == "pkg_config":
            pc = shutil.which("pkg-config")
            if not pc:
                return CapabilityObservation(spec.id, "missing", detail="pkg-config unavailable")
            module = str(args["module"])
            minimum = args.get("minimum_version")
            check_argv = (pc, f"--atleast-version={minimum}", module) if minimum else (pc, "--exists", module)
            check = _run(check_argv)
            if check is None or check.returncode != 0:
                return CapabilityObservation(spec.id, "missing", detail=f"pkg-config module {module} unavailable")
            version_result = _run((pc, "--modversion", module))
            version = version_result.stdout.strip() if version_result and version_result.returncode == 0 else None
            return CapabilityObservation(spec.id, "pass", version=version or None, detail=f"pkg-config module {module} available")

        if kind in {"any_command", "command_group"}:
            commands = tuple(str(x) for x in args["commands"])
            present = [cmd for cmd in commands if shutil.which(cmd)]
            ok = bool(present) if kind == "any_command" else len(present) == len(commands)
            return CapabilityObservation(spec.id, "pass" if ok else "missing", detail=", ".join(present) if present else "required command unavailable")

        if kind == "systemd_user":
            systemctl = shutil.which("systemctl")
            if not systemctl:
                return CapabilityObservation(spec.id, "missing", detail="systemctl unavailable")
            result = _run((systemctl, "--user", "show-environment"))
            return CapabilityObservation(spec.id, "pass" if result and result.returncode == 0 else "failed", detail="systemd user manager probe")

        if kind == "portal_unit":
            systemctl = shutil.which("systemctl")
            if not systemctl:
                return CapabilityObservation(spec.id, "missing", detail="systemctl unavailable")
            unit = str(args.get("unit", "xdg-desktop-portal-hyprland.service"))
            result = _run((systemctl, "--user", "list-unit-files", unit, "--no-legend", "--no-pager"))
            ok = bool(result and result.returncode == 0 and unit in result.stdout)
            return CapabilityObservation(spec.id, "pass" if ok else "missing", detail=f"user unit {unit}")

        if kind == "tesseract_language":
            exe = shutil.which("tesseract")
            if not exe:
                return CapabilityObservation(spec.id, "missing", detail="tesseract unavailable")
            result = _run((exe, "--list-langs"), timeout=6.0)
            language = str(args.get("language", "eng"))
            langs = set(result.stdout.split()) if result and result.returncode == 0 else set()
            return CapabilityObservation(spec.id, "pass" if language in langs else "missing", detail=f"tesseract language {language}")

        backend_command = {
            "networkmanager_backend": "nmcli",
            "bluetooth_backend": "bluetoothctl",
            "power_profiles_backend": "powerprofilesctl",
        }.get(kind)
        if backend_command:
            exe = shutil.which(backend_command)
            if not exe:
                return CapabilityObservation(spec.id, "missing", detail=f"{backend_command} unavailable")
            argv = {
                "networkmanager_backend": (exe, "-t", "-f", "STATE", "general"),
                "bluetooth_backend": (exe, "list"),
                "power_profiles_backend": (exe, "list"),
            }[kind]
            result = _run(argv, timeout=5.0)
            if result is None or result.returncode != 0:
                return CapabilityObservation(spec.id, "failed", detail=f"{backend_command} backend unreachable")
            if kind == "bluetooth_backend" and not result.stdout.strip():
                return CapabilityObservation(spec.id, "not_applicable", detail="no Bluetooth controller present")
            return CapabilityObservation(spec.id, "pass", detail=f"{backend_command} backend reachable")

        # Build/compile probes are intentionally not re-run by acceptance MVP.
        # Their installer-observed state remains in the candidate bundle; Doctor
        # marks the independent current observation unknown rather than lying.
        return CapabilityObservation(spec.id, "unknown", detail=f"acceptance MVP does not independently run {kind} probe")
    except Exception as exc:
        return CapabilityObservation(spec.id, "unknown", detail=f"probe error: {type(exc).__name__}: {exc}")


def _sha256(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def _observe_artifact(accepted: ReceiptArtifact) -> ArtifactObservation:
    path = Path(accepted.path)
    try:
        st = path.lstat()
    except FileNotFoundError:
        return ArtifactObservation(accepted.artifact_id, False)
    except OSError:
        return ArtifactObservation(accepted.artifact_id, False)
    mode = f"{stat.S_IMODE(st.st_mode):04o}"
    sha = None
    fingerprint = None
    try:
        if accepted.sha256 and stat.S_ISREG(st.st_mode) and not stat.S_ISLNK(st.st_mode):
            sha = _sha256(path)
        if accepted.immutable_fingerprint:
            fingerprint = fingerprint_path(path)
    except OSError:
        pass
    return ArtifactObservation(accepted.artifact_id, True, sha256=sha, immutable_fingerprint=fingerprint, mode=mode)


def _current_snapshot(registry: ManifestRegistry, candidate: InstalledStateReceipt) -> CurrentHealthSnapshot:
    capabilities = {spec.id: _probe_capability(spec) for spec in registry.capabilities_in_order()}
    artifacts = {aid: _observe_artifact(accepted) for aid, accepted in candidate.artifacts.items()}
    return CurrentHealthSnapshot(
        schema_version=1,
        captured_at=datetime.now(timezone.utc).isoformat(),
        activation_state=candidate.activation_state,
        runtime_health=candidate.runtime_health,
        capabilities=MappingProxyType(capabilities),
        artifacts=MappingProxyType(artifacts),
    )


def assess_candidate_install(registry: ManifestRegistry, payload: Mapping[str, Any]) -> AcceptanceAssessment:
    candidate = _candidate_receipt(payload, registry)
    snapshot = _current_snapshot(registry, candidate)
    forensic = analyze_forensics(registry, candidate, snapshot, health_context="doctor_manual", max_health_cost="cheap")
    findings: list[AcceptanceFinding] = []

    def add(code: str, severity: str, subject: str, summary: str) -> None:
        findings.append(AcceptanceFinding(code, severity, subject, summary))

    if candidate.install_health == "failed":
        add("RH_DOCTOR_CANDIDATE_INSTALL_FAILED", "critical", "installation", "installer verification already classified the candidate installation as failed")
    elif candidate.install_health == "degraded":
        add("RH_DOCTOR_CANDIDATE_INSTALL_DEGRADED", "warning", "installation", "installer verification classified the candidate installation as degraded")

    for component in candidate.components.values():
        if component.health in {"failed", "blocked"}:
            severity = "critical" if component.category in _CORE_CATEGORIES else "warning"
            add("RH_DOCTOR_COMPONENT_UNHEALTHY", severity, component.component_id, f"candidate component state is {component.health}")

    for capid, observation in snapshot.capabilities.items():
        spec = registry.capabilities.get(capid)
        if spec is None or observation.state in _SATISFIED or observation.state == "unknown":
            continue
        component = registry.components.get(spec.component_id or "")
        core = component is None or component.category in _CORE_CATEGORIES
        runtime = "runtime" in spec.lifecycle
        severity = "critical" if spec.requirement == "required" and runtime and core else "warning"
        add("RH_DOCTOR_CAPABILITY_UNHEALTHY", severity, capid, observation.detail or observation.state)

    for drift in forensic.drifts:
        severity = drift.severity
        component = registry.components.get(drift.component_id or "")
        if drift.kind.value == "dependency" and component is not None and component.category not in _CORE_CATEGORIES:
            if severity in {"critical", "error"}:
                severity = "warning"
        add(drift.error_code, severity, drift.subject_id, drift.summary)

    if candidate.activation_state == "pending_session_restart":
        add(
            "RH_DOCTOR_RUNTIME_PENDING_SESSION_RESTART",
            "info",
            "runtime.activation",
            "deployment checks can pass, but runtime Last Known Good must wait for a fresh Hyprland session",
        )

    critical = any(item.severity == "critical" for item in findings)
    warnings = any(item.severity in {"warning", "error"} for item in findings)
    if critical:
        recommendation = AcceptanceRecommendation.REVERT_RECOMMENDED
        summary = "Doctor found critical evidence that makes reverting the candidate installation advisable."
    elif warnings:
        recommendation = AcceptanceRecommendation.KEEP_WITH_WARNINGS
        summary = "Doctor found no critical blocker, but the candidate has warnings that should remain visible."
    else:
        recommendation = AcceptanceRecommendation.KEEP
        summary = "Doctor found no independent critical problem with the candidate installation."

    return AcceptanceAssessment(
        schema_version=1,
        recommendation=recommendation,
        transaction_id=candidate.transaction_id,
        realmheart_version=candidate.realmheart_version,
        manifest_digest=registry.digest,
        checked_artifacts=len(snapshot.artifacts),
        checked_capabilities=len(snapshot.capabilities),
        activation_state=candidate.activation_state,
        runtime_health=candidate.runtime_health,
        findings=tuple(findings),
        forensic_drift_count=len(forensic.drifts),
        forensic_incident_count=len(forensic.incidents),
        summary=summary,
    )
