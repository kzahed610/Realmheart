"""Repository/manifest drift checks used by CI and installer development."""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .manifest import ManifestRegistry, ParsedVersion, load_manifest


@dataclass(frozen=True)
class RepositoryValidation:
    ok: bool
    errors: tuple[str, ...]
    warnings: tuple[str, ...]


def _cmake_version(text: str) -> str | None:
    match = re.search(r"project\s*\(\s*Realmheart\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text, re.I | re.S)
    return match.group(1) if match else None


def _cmake_targets(text: str) -> set[str]:
    return set(re.findall(r"add_(?:executable|library)\s*\(\s*([A-Za-z0-9_.+\-]+)", text, re.I | re.S))


def _cmake_executable_targets(text: str) -> set[str]:
    return set(re.findall(r"add_executable\s*\(\s*([A-Za-z0-9_.+\-]+)", text, re.I | re.S))


def _cmake_output_names(text: str) -> dict[str, str]:
    names: dict[str, str] = {}
    pattern = re.compile(
        r"set_target_properties\s*\(\s*([A-Za-z0-9_.+\-]+)\s+PROPERTIES(?P<body>.*?)\)",
        re.I | re.S,
    )
    for match in pattern.finditer(text):
        output = re.search(r"\bOUTPUT_NAME\s+[\"']?([^\s\"')]+)", match.group("body"), re.I)
        if output:
            names[match.group(1)] = output.group(1)
    return names


def _realmheart_service_refs(root: Path) -> set[str]:
    refs: set[str] = set()
    hypr = root / "config" / "hypr"
    if not hypr.exists():
        return refs
    pattern = re.compile(r"\brealmheart[A-Za-z0-9_.-]*\.service\b")
    for path in hypr.rglob("*"):
        if not path.is_file() or path.suffix not in {".lua", ".conf", ".sh", ".service"}:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        refs.update(pattern.findall(text))
    return refs


def validate_repository(root: Path, registry: ManifestRegistry | None = None) -> RepositoryValidation:
    root = Path(root)
    registry = registry or load_manifest(root / "components")
    errors: list[str] = []
    warnings: list[str] = []

    cmake_path = root / "CMakeLists.txt"
    if not cmake_path.is_file():
        errors.append("CMakeLists.txt is missing")
        cmake_text = ""
    else:
        cmake_text = cmake_path.read_text(encoding="utf-8")

    version = _cmake_version(cmake_text)
    if version != registry.release_version:
        errors.append(f"CMake release {version or 'unavailable'} disagrees with manifest release {registry.release_version}")

    targets = _cmake_targets(cmake_text)
    executable_targets = _cmake_executable_targets(cmake_text)
    output_names = _cmake_output_names(cmake_text)
    for unit in registry.build_units.values():
        if unit.cmake_target and unit.cmake_target not in targets:
            errors.append(f"manifest build unit {unit.id} references missing CMake target {unit.cmake_target}")
            continue
        if not unit.cmake_target or unit.cmake_target not in executable_targets:
            continue
        installed_name = output_names.get(unit.cmake_target, unit.cmake_target)
        for artifact_id in unit.artifact_ids:
            artifact = registry.artifacts.get(artifact_id)
            if artifact is None or artifact.type != "executable":
                continue
            declared_name = Path(artifact.path).name
            if declared_name != installed_name:
                errors.append(
                    f"manifest artifact {artifact.id} declares executable basename {declared_name} "
                    f"but CMake target {unit.cmake_target} installs as {installed_name}"
                )

    for artifact in registry.artifacts.values():
        if artifact.source:
            source = root / artifact.source
            if not source.exists():
                errors.append(f"artifact {artifact.id} source is missing: {artifact.source}")

    evidence_components = {check.component_id for check in registry.health_checks.values()}
    evidence_components.update(
        capability.component_id for capability in registry.capabilities.values() if capability.component_id
    )
    for component in registry.components.values():
        if component.id not in evidence_components:
            errors.append(
                f"component {component.id} declares no health check or capability probe evidence"
            )

    declared_service_names = {
        Path(artifact.path).name
        for artifact in registry.artifacts.values()
        if artifact.type == "service" and Path(artifact.path).name.startswith("realmheart")
    }
    for service in sorted(_realmheart_service_refs(root)):
        if service not in declared_service_names:
            errors.append(f"shipped Hypr config references undeclared Realmheart service {service}")

    # Explicitly guard the historical personal/stale startup leaks that triggered
    # creation of this drift check.
    execs = root / "config" / "hypr" / "hyprland" / "execs.lua"
    if execs.is_file():
        text = execs.read_text(encoding="utf-8")
        forbidden = {
            "Bibata-Modern-Classic": "personal cursor choice",
            "easyeffects": "personal audio service",
            "gnome-keyring-daemon": "desktop keyring choice",
            "plasma-polkit-agent.service": "desktop-specific polkit agent",
            "start_geoclue_agent.sh": "stale GeoClue demo-agent startup",
            "/usr/lib/geoclue-2.0-386": "stale distro-specific GeoClue path",
        }
        for token, description in forbidden.items():
            if token in text:
                errors.append(f"Realmheart-owned startup still contains {description}: {token}")

    return RepositoryValidation(not errors, tuple(errors), tuple(warnings))
