"""Independent read-only post-install acceptance assessment.

This intentionally does not import ``realmheart_installer``.  Installer state is
input data, never executable Doctor behavior.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping

from realmheart_maintenance.fingerprint import (
    FingerprintLimitExceeded,
    MAX_SHA256_BYTES,
    fingerprint_path,
)
from realmheart_maintenance.forensics import (
    ArtifactObservation,
    CapabilityObservation,
    CurrentHealthSnapshot,
    InstalledStateReceipt,
    ObservationOutcome,
    ReceiptArtifact,
    ReceiptCapability,
    ReceiptComponent,
    analyze_forensics,
    artifact_integrity_fields,
    capability_state_satisfied,
    capability_version_required,
    classify_capability_version,
    version_evidence_line,
)
from realmheart_maintenance.manifest import (
    ManifestRegistry,
    ParsedVersion,
    VersionCompatibility,
    canonical_artifact_path_matches,
)

from .models import AcceptanceAssessment, AcceptanceFinding, AcceptanceRecommendation

CANDIDATE_SCHEMA_VERSION = 1
MAX_CANDIDATE_JSON_BYTES = 2 * 1024 * 1024
_CANDIDATE_READ_CHUNK_BYTES = 1024 * 1024
_BLOCKING_CATEGORIES = {"core", "essential", "fx"}
_CANDIDATE_COMPONENT_HEALTH = {
    "healthy", "degraded", "failed", "blocked", "unknown", "not_applicable", "pending_activation",
    "warning", "pending", "running", "skipped",
}
_CANDIDATE_CAPABILITY_STATES = {"pass", "missing", "failed", "not_applicable"}
_CANDIDATE_INSTALL_HEALTH = {"healthy", "degraded", "failed"}
_CANDIDATE_ACTIVATION_STATES = {"active", "pending_session_restart", "unknown", "failed"}
_CANDIDATE_RUNTIME_HEALTH = {"healthy", "degraded", "failed", "unknown"}


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


def _optional_text(value: Any, field: str) -> str | None:
    if value is None:
        return None
    return _text(value, field)


def _optional_digest(value: Any, field: str) -> str | None:
    text = _optional_text(value, field)
    if text is not None and not re.fullmatch(r"[0-9a-fA-F]{64}", text):
        raise DoctorAcceptanceError(f"{field} must be a 64-character hexadecimal SHA-256 digest or null")
    return text


def _string_list(value: Any, field: str) -> tuple[str, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise DoctorAcceptanceError(f"{field} must be an array of strings")
    return tuple(value)


def _candidate_ids(records: Mapping[Any, Any], field: str) -> set[str]:
    ids: set[str] = set()
    for raw_id in records:
        if not isinstance(raw_id, str) or not raw_id:
            raise DoctorAcceptanceError(f"{field} contains an invalid record id")
        ids.add(raw_id)
    return ids


def _validate_coverage(
    records: Mapping[Any, Any],
    *,
    expected: set[str],
    required: set[str],
    field: str,
) -> None:
    actual = _candidate_ids(records, field)
    unexpected = sorted(actual - expected)
    if unexpected:
        raise DoctorAcceptanceError(f"{field} contains non-canonical record(s): {', '.join(unexpected)}")
    missing = sorted(required - actual)
    if missing:
        raise DoctorAcceptanceError(f"{field} is missing required record(s): {', '.join(missing)}")


def _canonical_path_matches(declared: str, candidate: str) -> bool:
    """Accept only exact paths resolved from explicit canonical variables."""

    return canonical_artifact_path_matches(declared, candidate)


def load_candidate_bundle(
    path: Path,
    *,
    max_bytes: int = MAX_CANDIDATE_JSON_BYTES,
) -> Mapping[str, Any]:
    path = Path(path)
    if type(max_bytes) is not int or max_bytes < 0:
        raise DoctorAcceptanceError("candidate bundle byte limit must be a non-negative integer")
    if max_bytes > MAX_CANDIDATE_JSON_BYTES:
        raise DoctorAcceptanceError(
            f"candidate bundle byte limit exceeds hard limit {MAX_CANDIDATE_JSON_BYTES}"
        )
    try:
        st = path.lstat()
        if not stat.S_ISREG(st.st_mode):
            raise DoctorAcceptanceError(f"candidate bundle must be a regular non-symlink file: {path}")
        if st.st_size > max_bytes:
            raise DoctorAcceptanceError(
                f"candidate bundle exceeds the {max_bytes}-byte observation limit"
            )
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
        descriptor = os.open(os.fspath(path), flags)
        with os.fdopen(descriptor, "rb", closefd=True) as handle:
            chunks: list[bytes] = []
            total = 0
            while total <= max_bytes:
                chunk = handle.read(min(_CANDIDATE_READ_CHUNK_BYTES, max_bytes - total + 1))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
            raw = b"".join(chunks)
        if len(raw) > max_bytes:
            raise DoctorAcceptanceError(
                f"candidate bundle exceeds the {max_bytes}-byte observation limit"
            )
        payload = json.loads(raw.decode("utf-8"))
    except DoctorAcceptanceError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise DoctorAcceptanceError(f"cannot read candidate bundle: {exc}") from exc
    return _mapping(payload, "candidate bundle")


def _candidate_receipt(payload: Mapping[str, Any], registry: ManifestRegistry) -> InstalledStateReceipt:
    if not isinstance(payload, Mapping):
        raise DoctorAcceptanceError("candidate bundle must be an object")
    if type(payload.get("schema_version")) is not int or payload.get("schema_version") != CANDIDATE_SCHEMA_VERSION:
        raise DoctorAcceptanceError(f"unsupported candidate schema {payload.get('schema_version')!r}")
    if payload.get("kind") != "realmheart_install_candidate":
        raise DoctorAcceptanceError("candidate bundle kind must be realmheart_install_candidate")
    if payload.get("manifest_set_sha256") != registry.digest:
        raise DoctorAcceptanceError("candidate manifest digest does not match the canonical manifest")
    if type(payload.get("manifest_schema_version")) is not int or payload.get("manifest_schema_version") != registry.schema_version:
        raise DoctorAcceptanceError("candidate manifest schema does not match the canonical manifest")

    components_raw = _mapping(payload.get("components"), "components")
    dependencies_raw = _mapping(payload.get("dependencies"), "dependencies")
    artifacts_raw = _mapping(payload.get("artifacts"), "artifacts")
    required_component_ids = {
        spec.component_id
        for spec in registry.capabilities.values()
        if spec.component_id is not None
        and spec.requirement in {"required", "component"}
        and (
            spec.requirement == "required"
            or registry.components[spec.component_id].category in _BLOCKING_CATEGORIES
        )
    }
    required_component_ids.update(
        spec.component_id for spec in registry.artifacts.values() if spec.required
    )
    required_component_ids.update(
        component_id
        for component_id, spec in registry.components.items()
        if spec.category in _BLOCKING_CATEGORIES
    )
    _validate_coverage(
        components_raw,
        expected=set(registry.components),
        required=required_component_ids,
        field="components",
    )
    _validate_coverage(
        dependencies_raw,
        expected=set(registry.capabilities),
        required={
            capid
            for capid, spec in registry.capabilities.items()
            if spec.requirement == "required"
            or (
                spec.requirement == "component"
                and spec.component_id is not None
                and registry.components[spec.component_id].category in _BLOCKING_CATEGORIES
            )
        },
        field="dependencies",
    )
    _validate_coverage(
        artifacts_raw,
        expected=set(registry.artifacts),
        required={aid for aid, spec in registry.artifacts.items() if spec.required},
        field="artifacts",
    )

    components: dict[str, ReceiptComponent] = {}
    for cid, raw_value in components_raw.items():
        canonical = registry.components.get(cid)
        if canonical is None:
            raise DoctorAcceptanceError(f"component {cid} is not declared by the canonical manifest")
        raw = _mapping(raw_value, f"component {cid}")
        display_name = _text(raw.get("display_name"), f"component {cid}.display_name")
        if display_name != canonical.name:
            raise DoctorAcceptanceError(f"component {cid}.display_name does not match the canonical manifest")
        category = _text(raw.get("category"), f"component {cid}.category")
        if category != canonical.category:
            raise DoctorAcceptanceError(f"component {cid}.category does not match the canonical manifest")
        health = _text(raw.get("health"), f"component {cid}.health")
        if health not in _CANDIDATE_COMPONENT_HEALTH:
            raise DoctorAcceptanceError(f"component {cid}.health has invalid state {health!r}")
        blocked_by = _string_list(raw.get("blocked_by", []), f"component {cid}.blocked_by")
        artifact_ids = _string_list(raw.get("artifact_ids", []), f"component {cid}.artifact_ids")
        build_unit_ids = _string_list(raw.get("build_unit_ids", []), f"component {cid}.build_unit_ids")
        if "warnings" in raw:
            _string_list(raw["warnings"], f"component {cid}.warnings")
        for aid in artifact_ids:
            artifact = registry.artifacts.get(aid)
            if artifact is None or artifact.component_id != cid:
                raise DoctorAcceptanceError(f"component {cid}.artifact_ids contains a non-canonical artifact")
        for bid in build_unit_ids:
            build_unit = registry.build_units.get(bid)
            if build_unit is None or cid not in build_unit.component_ids:
                raise DoctorAcceptanceError(f"component {cid}.build_unit_ids contains a non-canonical build unit")
        components[cid] = ReceiptComponent(
            component_id=cid,
            display_name=display_name,
            category=category,
            health=health,
            blocked_by=blocked_by,
            artifact_ids=artifact_ids,
            build_unit_ids=build_unit_ids,
        )

    capabilities: dict[str, ReceiptCapability] = {}
    for capid, raw_value in dependencies_raw.items():
        canonical = registry.capabilities.get(capid)
        if canonical is None:
            raise DoctorAcceptanceError(f"dependency {capid} is not declared by the canonical manifest")
        raw = _mapping(raw_value, f"dependency {capid}")
        component_id = raw.get("component_id")
        if component_id is not None and not isinstance(component_id, str):
            raise DoctorAcceptanceError(f"dependency {capid}.component_id must be a string or null")
        if component_id != canonical.component_id:
            raise DoctorAcceptanceError(f"dependency {capid}.component_id does not match the canonical manifest")
        requirement = _text(raw.get("requirement"), f"dependency {capid}.requirement")
        if requirement != canonical.requirement:
            raise DoctorAcceptanceError(f"dependency {capid}.requirement does not match the canonical manifest")
        lifecycle = _string_list(raw.get("lifecycle", []), f"dependency {capid}.lifecycle")
        if lifecycle != canonical.lifecycle:
            raise DoctorAcceptanceError(f"dependency {capid}.lifecycle does not match the canonical manifest")
        state = _text(raw.get("state"), f"dependency {capid}.state")
        if state not in _CANDIDATE_CAPABILITY_STATES:
            raise DoctorAcceptanceError(f"dependency {capid}.state has invalid state {state!r}")
        capabilities[capid] = ReceiptCapability(
            capability_id=capid,
            component_id=component_id,
            requirement=requirement,
            lifecycle=lifecycle,
            state=state,
            version=_optional_text(raw.get("version"), f"dependency {capid}.version"),
        )

    artifacts: dict[str, ReceiptArtifact] = {}
    for aid, raw_value in artifacts_raw.items():
        canonical = registry.artifacts.get(aid)
        if canonical is None:
            raise DoctorAcceptanceError(f"artifact {aid} is not declared by the canonical manifest")
        raw = _mapping(raw_value, f"artifact {aid}")
        component_id = _text(raw.get("component_id"), f"artifact {aid}.component_id")
        if component_id != canonical.component_id:
            raise DoctorAcceptanceError(f"artifact {aid}.component_id does not match the canonical manifest")
        path = _text(raw.get("path"), f"artifact {aid}.path")
        if not _canonical_path_matches(canonical.path, path):
            raise DoctorAcceptanceError(f"artifact {aid}.path is not authorized by the canonical manifest")
        artifact_type = _text(raw.get("type"), f"artifact {aid}.type")
        if artifact_type != canonical.type:
            raise DoctorAcceptanceError(f"artifact {aid}.type does not match the canonical manifest")
        ownership = _text(raw.get("ownership"), f"artifact {aid}.ownership")
        if ownership != canonical.ownership:
            raise DoctorAcceptanceError(f"artifact {aid}.ownership does not match the canonical manifest")
        if "required" in raw:
            required = raw["required"]
            if type(required) is not bool or required != canonical.required:
                raise DoctorAcceptanceError(f"artifact {aid}.required does not match the canonical manifest")
        mode = _optional_text(raw.get("mode"), f"artifact {aid}.mode")
        if mode is not None and (len(mode) != 4 or any(char not in "01234567" for char in mode)):
            raise DoctorAcceptanceError(f"artifact {aid}.mode must be four octal digits or null")
        if canonical.mode is not None and mode is not None and mode != canonical.mode:
            raise DoctorAcceptanceError(f"artifact {aid}.mode does not match the canonical manifest")
        artifacts[aid] = ReceiptArtifact(
            artifact_id=aid,
            component_id=component_id,
            path=path,
            artifact_type=artifact_type,
            ownership=ownership,
            mode=mode,
            sha256=_optional_digest(raw.get("sha256"), f"artifact {aid}.sha256"),
            immutable_fingerprint=_optional_digest(raw.get("immutable_fingerprint"), f"artifact {aid}.immutable_fingerprint"),
        )

    for aid, artifact in artifacts.items():
        component = components.get(artifact.component_id)
        if component is None or aid not in component.artifact_ids:
            raise DoctorAcceptanceError(f"artifact {aid} is not covered by its canonical component record")
    for aid, canonical in registry.artifacts.items():
        if canonical.required and aid not in components[canonical.component_id].artifact_ids:
            raise DoctorAcceptanceError(f"required artifact {aid} is omitted from its component coverage")

    install_health = _text(payload.get("install_health"), "install_health")
    activation_state = _text(payload.get("activation_state"), "activation_state")
    runtime_health = _text(payload.get("runtime_health"), "runtime_health")
    if install_health not in _CANDIDATE_INSTALL_HEALTH:
        raise DoctorAcceptanceError(f"install_health has invalid state {install_health!r}")
    if activation_state not in _CANDIDATE_ACTIVATION_STATES:
        raise DoctorAcceptanceError(f"activation_state has invalid state {activation_state!r}")
    if runtime_health not in _CANDIDATE_RUNTIME_HEALTH:
        raise DoctorAcceptanceError(f"runtime_health has invalid state {runtime_health!r}")

    return InstalledStateReceipt(
        schema_version=2,
        realmheart_version=_text(payload.get("realmheart_version"), "realmheart_version"),
        manifest_schema_version=payload["manifest_schema_version"],
        manifest_digest=_text(payload.get("manifest_set_sha256"), "manifest_set_sha256"),
        installer_version=_text(payload.get("installer_version"), "installer_version"),
        transaction_id=_text(payload.get("transaction_id"), "transaction_id"),
        disposition="candidate",
        install_health=install_health,
        activation_state=activation_state,
        runtime_health=runtime_health,
        components=MappingProxyType(components),
        capabilities=MappingProxyType(capabilities),
        artifacts=MappingProxyType(artifacts),
    )


@dataclass(frozen=True)
class _ProbeFailure:
    kind: str
    detail: str


def _run(argv: tuple[str, ...], *, timeout: float = 4.0) -> subprocess.CompletedProcess[str] | _ProbeFailure:
    try:
        return subprocess.run(argv, text=True, capture_output=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return _ProbeFailure("timeout", f"command exceeded the {timeout:g}s observation deadline")
    except (OSError, UnicodeError) as exc:
        return _ProbeFailure("io_error", f"{type(exc).__name__}: {exc}")


def _which(executable: str) -> str | None | _ProbeFailure:
    try:
        return shutil.which(executable)
    except OSError as exc:
        return _ProbeFailure("io_error", f"{type(exc).__name__}: {exc}")


def _unknown_probe(spec, failure: _ProbeFailure | None = None) -> CapabilityObservation:
    if failure is None:
        detail = "probe result was not available"
    else:
        detail = f"{failure.kind}: {failure.detail}"
    return CapabilityObservation(spec.id, "unknown", detail=detail)


def _probe_version_evidence(spec, *values: Any) -> str | None:
    for value in values:
        evidence = version_evidence_line(spec, value)
        if evidence is not None:
            return evidence
    return None


def _version_required(spec, registry: ManifestRegistry | None) -> bool:
    if registry is not None:
        return capability_version_required(registry, spec)
    args = spec.probe.args
    return bool(
        args.get("version_argv")
        or args.get("minimum_version")
        or args.get("maximum_version")
        or args.get("exact_version")
        or args.get("tested_ranges")
        or args.get("known_incompatible")
    )


def _classify_probe_version(
    spec,
    version: str | None,
    registry: ManifestRegistry | None,
) -> VersionCompatibility:
    evidence = version_evidence_line(spec, version)
    if evidence is None or ParsedVersion.parse(evidence) is None:
        return VersionCompatibility.UNPARSEABLE
    if registry is None:
        return VersionCompatibility.SATISFIED_UNTESTED
    return classify_capability_version(registry, spec, evidence)


def _probe_capability(
    spec,
    *,
    registry: ManifestRegistry | None = None,
) -> CapabilityObservation:
    kind = spec.probe.kind
    args = spec.probe.args
    if kind == "executable":
        executable = str(args["executable"])
        path = _which(executable)
        if isinstance(path, _ProbeFailure):
            return _unknown_probe(spec, path)
        if not path:
            return CapabilityObservation(spec.id, "missing", detail=f"{executable} not found")
        version = None
        version_argv = tuple(str(x) for x in args.get("version_argv", ()))
        version_required = _version_required(spec, registry)
        if version_required and not version_argv:
            return CapabilityObservation(
                spec.id,
                "unknown",
                detail=f"cannot establish {executable} version: no version probe is declared",
            )
        if version_argv:
            result = _run((path, *version_argv))
            if isinstance(result, _ProbeFailure) or result is None:
                return _unknown_probe(spec, result if isinstance(result, _ProbeFailure) else None)
            if result.returncode != 0:
                return CapabilityObservation(spec.id, "failed", detail=f"cannot query {executable} version")
            version = _probe_version_evidence(spec, result.stdout, result.stderr)
            compatibility = _classify_probe_version(spec, version, registry)
            if compatibility is VersionCompatibility.UNPARSEABLE:
                return CapabilityObservation(
                    spec.id,
                    "unknown",
                    detail=f"{executable} returned no parseable version evidence",
                )
            if compatibility is VersionCompatibility.INCOMPATIBLE:
                return CapabilityObservation(
                    spec.id,
                    "failed",
                    version=version,
                    detail=f"{executable} version violates the canonical compatibility contract",
                )
        return CapabilityObservation(spec.id, "pass", version=version, detail=f"{executable} available")

    if kind == "pkg_config":
        pc = _which("pkg-config")
        if isinstance(pc, _ProbeFailure):
            return _unknown_probe(spec, pc)
        if not pc:
            return CapabilityObservation(spec.id, "missing", detail="pkg-config unavailable")
        module = str(args["module"])
        minimum = args.get("minimum_version")
        check_argv = (pc, f"--atleast-version={minimum}", module) if minimum else (pc, "--exists", module)
        check = _run(check_argv)
        if isinstance(check, _ProbeFailure) or check is None:
            return _unknown_probe(spec, check if isinstance(check, _ProbeFailure) else None)
        if check.returncode != 0:
            return CapabilityObservation(spec.id, "missing", detail=f"pkg-config module {module} unavailable")
        version_result = _run((pc, "--modversion", module))
        if isinstance(version_result, _ProbeFailure) or version_result is None:
            return _unknown_probe(spec, version_result if isinstance(version_result, _ProbeFailure) else None)
        if version_result.returncode != 0:
            return CapabilityObservation(spec.id, "unknown", detail=f"cannot query pkg-config module {module} version")
        version = _probe_version_evidence(spec, version_result.stdout, version_result.stderr)
        version_required = _version_required(spec, registry)
        if version_required:
            compatibility = _classify_probe_version(spec, version, registry)
            if compatibility is VersionCompatibility.UNPARSEABLE:
                return CapabilityObservation(
                    spec.id,
                    "unknown",
                    detail=f"pkg-config module {module} returned no parseable version evidence",
                )
            if compatibility is VersionCompatibility.INCOMPATIBLE:
                return CapabilityObservation(
                    spec.id,
                    "failed",
                    version=version,
                    detail=f"pkg-config module {module} version violates the canonical compatibility contract",
                )
        return CapabilityObservation(spec.id, "pass", version=version, detail=f"pkg-config module {module} available")

    if kind in {"any_command", "command_group"}:
        commands = tuple(str(x) for x in args["commands"])
        present: list[str] = []
        for command in commands:
            path = _which(command)
            if isinstance(path, _ProbeFailure):
                return _unknown_probe(spec, path)
            if path:
                present.append(command)
        ok = bool(present) if kind == "any_command" else len(present) == len(commands)
        return CapabilityObservation(spec.id, "pass" if ok else "missing", detail=", ".join(present) if present else "required command unavailable")

    if kind == "systemd_user":
        systemctl = _which("systemctl")
        if isinstance(systemctl, _ProbeFailure):
            return _unknown_probe(spec, systemctl)
        if not systemctl:
            return CapabilityObservation(spec.id, "missing", detail="systemctl unavailable")
        result = _run((systemctl, "--user", "show-environment"))
        if isinstance(result, _ProbeFailure) or result is None:
            return _unknown_probe(spec, result if isinstance(result, _ProbeFailure) else None)
        return CapabilityObservation(spec.id, "pass" if result.returncode == 0 else "failed", detail="systemd user manager probe")

    if kind == "portal_unit":
        systemctl = _which("systemctl")
        if isinstance(systemctl, _ProbeFailure):
            return _unknown_probe(spec, systemctl)
        if not systemctl:
            return CapabilityObservation(spec.id, "missing", detail="systemctl unavailable")
        unit = str(args.get("unit", "xdg-desktop-portal-hyprland.service"))
        result = _run((systemctl, "--user", "list-unit-files", unit, "--no-legend", "--no-pager"))
        if isinstance(result, _ProbeFailure) or result is None:
            return _unknown_probe(spec, result if isinstance(result, _ProbeFailure) else None)
        ok = result.returncode == 0 and unit in (result.stdout or "")
        return CapabilityObservation(spec.id, "pass" if ok else "missing", detail=f"user unit {unit}")

    if kind == "tesseract_language":
        exe = _which("tesseract")
        if isinstance(exe, _ProbeFailure):
            return _unknown_probe(spec, exe)
        if not exe:
            return CapabilityObservation(spec.id, "missing", detail="tesseract unavailable")
        result = _run((exe, "--list-langs"), timeout=6.0)
        if isinstance(result, _ProbeFailure) or result is None:
            return _unknown_probe(spec, result if isinstance(result, _ProbeFailure) else None)
        if result.returncode != 0:
            return CapabilityObservation(spec.id, "unknown", detail="tesseract language probe failed")
        language = str(args.get("language", "eng"))
        langs = set((result.stdout or "").split())
        return CapabilityObservation(spec.id, "pass" if language in langs else "missing", detail=f"tesseract language {language}")

    backend_command = {
        "networkmanager_backend": "nmcli",
        "bluetooth_backend": "bluetoothctl",
        "power_profiles_backend": "powerprofilesctl",
    }.get(kind)
    if backend_command:
        exe = _which(backend_command)
        if isinstance(exe, _ProbeFailure):
            return _unknown_probe(spec, exe)
        if not exe:
            return CapabilityObservation(spec.id, "missing", detail=f"{backend_command} unavailable")
        argv = {
            "networkmanager_backend": (exe, "-t", "-f", "STATE", "general"),
            "bluetooth_backend": (exe, "list"),
            "power_profiles_backend": (exe, "list"),
        }[kind]
        result = _run(argv, timeout=5.0)
        if isinstance(result, _ProbeFailure) or result is None:
            return _unknown_probe(spec, result if isinstance(result, _ProbeFailure) else None)
        if result.returncode != 0:
            return CapabilityObservation(spec.id, "failed", detail=f"{backend_command} backend unreachable")
        if kind == "bluetooth_backend" and not (result.stdout or "").strip():
            return CapabilityObservation(spec.id, "not_applicable", detail="no Bluetooth controller present")
        return CapabilityObservation(spec.id, "pass", detail=f"{backend_command} backend reachable")

    # Build/compile probes are intentionally not re-run by acceptance MVP.
    # Their installer-observed state remains in the candidate bundle; Doctor
    # marks the independent current observation unknown rather than lying.
    return CapabilityObservation(spec.id, "unknown", detail=f"acceptance MVP does not independently run {kind} probe")


def _sha256(path: Path, *, max_bytes: int = MAX_SHA256_BYTES) -> str:
    if type(max_bytes) is not int or max_bytes < 0:
        raise ValueError("sha256 byte limit must be a non-negative integer")
    if max_bytes > MAX_SHA256_BYTES:
        raise ValueError(f"sha256 byte limit exceeds hard limit {MAX_SHA256_BYTES}")
    hasher = hashlib.sha256()
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open(os.fspath(path), flags)
    used_bytes = 0
    with os.fdopen(descriptor, "rb", closefd=True) as handle:
        while True:
            remaining = max_bytes - used_bytes
            chunk = handle.read(min(1024 * 1024, remaining + 1))
            if not chunk:
                break
            if len(chunk) > remaining:
                raise FingerprintLimitExceeded("sha256_bytes", max_bytes, used_bytes + len(chunk))
            hasher.update(chunk)
            used_bytes += len(chunk)
    return hasher.hexdigest()


def _filesystem_type(mode: int) -> str:
    if stat.S_ISREG(mode):
        return "file"
    if stat.S_ISDIR(mode):
        return "directory"
    if stat.S_ISLNK(mode):
        return "symlink"
    return "other"


def _observe_artifact(
    accepted: ReceiptArtifact,
    *,
    required_fields: frozenset[str] = frozenset(),
) -> ArtifactObservation:
    path = Path(accepted.path)
    try:
        st = path.lstat()
    except FileNotFoundError:
        return ArtifactObservation(
            accepted.artifact_id,
            False,
            path=accepted.path,
            filesystem_type=None,
            outcome=ObservationOutcome.MISSING,
        )
    except OSError as exc:
        return ArtifactObservation(
            accepted.artifact_id,
            False,
            path=accepted.path,
            error=f"{type(exc).__name__}: {exc}",
            outcome=ObservationOutcome.UNKNOWN,
        )
    mode = f"{stat.S_IMODE(st.st_mode):04o}"
    filesystem_type = _filesystem_type(st.st_mode)
    sha = None
    fingerprint = None
    try:
        if "sha256" in required_fields and filesystem_type == "file":
            sha = _sha256(path)
        if "immutable_fingerprint" in required_fields:
            fingerprint = fingerprint_path(path)
    except FingerprintLimitExceeded as exc:
        return ArtifactObservation(
            accepted.artifact_id,
            True,
            path=accepted.path,
            sha256=sha,
            immutable_fingerprint=fingerprint,
            mode=mode,
            filesystem_type=filesystem_type,
            error=str(exc),
            outcome=ObservationOutcome.LIMIT_EXCEEDED,
        )
    except (OSError, UnicodeError, RecursionError) as exc:
        return ArtifactObservation(
            accepted.artifact_id,
            True,
            path=accepted.path,
            sha256=sha,
            immutable_fingerprint=fingerprint,
            mode=mode,
            filesystem_type=filesystem_type,
            error=f"{type(exc).__name__}: {exc}",
            outcome=ObservationOutcome.UNKNOWN,
        )
    return ArtifactObservation(
        accepted.artifact_id,
        True,
        path=accepted.path,
        sha256=sha,
        immutable_fingerprint=fingerprint,
        mode=mode,
        filesystem_type=filesystem_type,
        outcome=ObservationOutcome.OBSERVED,
    )


def _current_snapshot(registry: ManifestRegistry, candidate: InstalledStateReceipt) -> CurrentHealthSnapshot:
    capabilities = {
        spec.id: _probe_capability(spec, registry=registry)
        for spec in registry.capabilities_in_order()
    }
    artifacts = {
        aid: _observe_artifact(
            ReceiptArtifact(
                artifact_id=aid,
                component_id=registry.artifacts[aid].component_id,
                path=accepted.path,
                artifact_type=registry.artifacts[aid].type,
                ownership=registry.artifacts[aid].ownership,
                mode=accepted.mode,
                sha256=accepted.sha256,
                immutable_fingerprint=accepted.immutable_fingerprint,
            ),
            required_fields=artifact_integrity_fields(registry.artifacts[aid]),
        )
        for aid, accepted in candidate.artifacts.items()
    }
    return CurrentHealthSnapshot(
        schema_version=2,
        captured_at=datetime.now(timezone.utc).isoformat(),
        activation_state=candidate.activation_state,
        runtime_health=candidate.runtime_health,
        capabilities=MappingProxyType(capabilities),
        artifacts=MappingProxyType(artifacts),
    )


def _capability_failure_severity(registry: ManifestRegistry, spec) -> str:
    component = registry.components.get(spec.component_id or "")
    blocking = component is None or component.category in _BLOCKING_CATEGORIES
    if blocking and (
        spec.requirement == "component"
        or (spec.requirement == "required" and "runtime" in spec.lifecycle)
    ):
        return "critical"
    return "warning"


def assess_candidate_install(registry: ManifestRegistry, payload: Mapping[str, Any]) -> AcceptanceAssessment:
    candidate = _candidate_receipt(payload, registry)
    snapshot = _current_snapshot(registry, candidate)
    forensic = analyze_forensics(registry, candidate, snapshot, health_context="doctor_manual", max_health_cost="cheap")
    findings: list[AcceptanceFinding] = []

    def add(code: str, severity: str, subject: str, summary: str) -> None:
        findings.append(AcceptanceFinding(code, severity, subject, summary))

    uncertain = False
    if candidate.install_health == "failed":
        add("RH_DOCTOR_CANDIDATE_INSTALL_FAILED", "critical", "installation", "installer verification already classified the candidate installation as failed")
    elif candidate.install_health == "degraded":
        add("RH_DOCTOR_CANDIDATE_INSTALL_DEGRADED", "warning", "installation", "installer verification classified the candidate installation as degraded")

    for capid, accepted in candidate.capabilities.items():
        spec = registry.capabilities.get(capid)
        if spec is None:
            continue
        if capability_state_satisfied(registry, spec, accepted.state):
            continue
        if accepted.state == "not_applicable":
            uncertain = True
            add(
                "RH_DOCTOR_CAPABILITY_UNCERTAIN",
                "warning",
                capid,
                "blocking capability was marked not_applicable instead of being established",
            )
            continue
        add(
            "RH_DOCTOR_CAPABILITY_UNHEALTHY",
            _capability_failure_severity(registry, spec),
            capid,
            f"candidate capability state is {accepted.state}",
        )

    for component in candidate.components.values():
        if component.health in {"failed", "blocked"}:
            severity = "critical" if component.category in _BLOCKING_CATEGORIES else "warning"
            add("RH_DOCTOR_COMPONENT_UNHEALTHY", severity, component.component_id, f"candidate component state is {component.health}")
        elif component.health == "degraded":
            add("RH_DOCTOR_COMPONENT_DEGRADED", "warning", component.component_id, "candidate component state is degraded")
        elif component.health == "warning":
            add("RH_DOCTOR_COMPONENT_DEGRADED", "warning", component.component_id, "candidate component state is warning")
        elif component.health in {"unknown", "pending", "running", "skipped", "not_applicable", "pending_activation"}:
            blocking = component.category in _BLOCKING_CATEGORIES
            uncertain = uncertain or blocking
            add(
                "RH_DOCTOR_COMPONENT_UNCERTAIN",
                "warning",
                component.component_id,
                f"candidate component state is not established: {component.health}",
            )

    for capid, observation in snapshot.capabilities.items():
        spec = registry.capabilities.get(capid)
        if spec is None or capability_state_satisfied(registry, spec, observation.state) or observation.state == "unknown":
            continue
        severity = _capability_failure_severity(registry, spec)
        add("RH_DOCTOR_CAPABILITY_UNHEALTHY", severity, capid, observation.detail or observation.state)

    for drift in forensic.drifts:
        if drift.error_code in {
            "RH_FORENSIC_DEPENDENCY_UNKNOWN",
            "RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN",
        }:
            capability = registry.capabilities.get(drift.subject_id)
            component = registry.components.get(capability.component_id) if capability and capability.component_id else None
            blocking_component = component is not None and component.category in _BLOCKING_CATEGORIES
            critical_evidence = capability is not None and (
                capability.requirement == "required"
                or (capability.requirement == "component" and blocking_component)
            )
            if critical_evidence:
                uncertain = True
            if drift.error_code == "RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN":
                add("RH_DOCTOR_CAPABILITY_VERSION_UNKNOWN", "warning", drift.subject_id, drift.summary)
            elif critical_evidence:
                add("RH_DOCTOR_CAPABILITY_UNCERTAIN", "warning", drift.subject_id, drift.summary)
        if drift.error_code in {
            "RH_FORENSIC_ARTIFACT_UNKNOWN",
            "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN",
        }:
            artifact = registry.artifacts.get(drift.subject_id)
            if artifact is not None and artifact.required:
                uncertain = True
                if "receipt evidence is incomplete" in drift.summary:
                    add("RH_DOCTOR_ARTIFACT_INTEGRITY_UNKNOWN", "warning", drift.subject_id, drift.summary)
        severity = drift.severity
        component = registry.components.get(drift.component_id or "")
        if drift.kind.value == "dependency" and component is not None and component.category not in _BLOCKING_CATEGORIES:
            if severity in {"critical", "error"}:
                severity = "warning"
        elif drift.kind.value == "artifact" and severity in {"critical", "error"}:
            artifact = registry.artifacts.get(drift.subject_id)
            if artifact is not None:
                artifact_component = registry.components.get(artifact.component_id)
                if artifact.required and artifact_component is not None and artifact_component.category in _BLOCKING_CATEGORIES:
                    severity = "critical"
        add(drift.error_code, severity, drift.subject_id, drift.summary)

    if candidate.activation_state == "pending_session_restart":
        add(
            "RH_DOCTOR_RUNTIME_PENDING_SESSION_RESTART",
            "info",
            "runtime.activation",
            "deployment checks can pass, but runtime Last Known Good must wait for a fresh Hyprland session",
        )
    elif candidate.activation_state == "unknown":
        uncertain = True
        add(
            "RH_DOCTOR_RUNTIME_ACTIVATION_UNKNOWN",
            "warning",
            "runtime.activation",
            "candidate activation state was not independently established",
        )
    elif candidate.activation_state == "failed":
        add(
            "RH_DOCTOR_RUNTIME_ACTIVATION_FAILED",
            "critical",
            "runtime.activation",
            "candidate activation state is failed",
        )

    if candidate.runtime_health == "unknown":
        uncertain = True
        add(
            "RH_DOCTOR_RUNTIME_HEALTH_UNKNOWN",
            "warning",
            "runtime.health",
            "candidate runtime health was not independently established",
        )
    elif candidate.runtime_health == "failed":
        add(
            "RH_DOCTOR_RUNTIME_HEALTH_FAILED",
            "critical",
            "runtime.health",
            "candidate runtime health is failed",
        )
    elif candidate.runtime_health == "degraded":
        add(
            "RH_DOCTOR_RUNTIME_HEALTH_DEGRADED",
            "warning",
            "runtime.health",
            "candidate runtime health is degraded",
        )

    critical = any(item.severity == "critical" for item in findings)
    warnings = any(item.severity in {"warning", "error"} for item in findings)
    if critical:
        recommendation = AcceptanceRecommendation.REVERT_RECOMMENDED
        summary = "Doctor found critical evidence that makes reverting the candidate installation advisable."
    elif uncertain:
        recommendation = AcceptanceRecommendation.INDETERMINATE
        summary = "Doctor could not independently establish all required candidate observations."
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
