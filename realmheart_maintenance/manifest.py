"""Canonical Realmheart TOML manifest loader and validator.

The manifest describes product facts only.  Tool-specific behavior stays in
independent binding registries keyed by stable IDs.
"""

from __future__ import annotations

import hashlib
import re
import tomllib
from dataclasses import dataclass
from enum import Enum
from pathlib import Path, PurePosixPath
from types import MappingProxyType
from typing import Any, Mapping

SUPPORTED_SCHEMA_VERSION = 1
_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
_VERSION_RE = re.compile(r"(?<!\d)(\d+)\.(\d+)(?:\.(\d+))?")
_ALLOWED_CATEGORIES = {"core", "essential", "qol", "optional", "experimental", "fx"}
_ALLOWED_STAGES = {"foundation", "core_shell", "integration", "services", "desktop", "terminal", "verification"}
_ALLOWED_REQUIREMENTS = {"required", "component", "soft"}
_ALLOWED_LIFECYCLES = {"build", "install", "runtime", "verification", "repair", "ordering"}
_ALLOWED_ARTIFACT_TYPES = {"executable", "library", "file", "directory", "asset", "config", "service", "pam", "generated"}
_ALLOWED_OWNERSHIP = {"user", "system", "release", "shared"}
_ALLOWED_HEALTH_CHECKS = {
    "artifact_exists", "artifact_executable", "file_hash_matches", "version_probe",
    "runtime_probe", "config_parse", "socket_reachable", "process_start_smoke",
}
_ALLOWED_COST = {"cheap", "normal", "expensive"}
_ALLOWED_SIDE_EFFECTS = {"none", "read_only", "starts_component", "other"}
_ALLOWED_PROBES = {
    "executable", "pkg_config", "cxx26", "opencv_cmake", "pam_link", "cmake_gtest",
    "tesseract_language", "networkmanager_backend", "bluetooth_backend",
    "power_profiles_backend", "systemd_user", "portal_unit", "any_command", "command_group",
}
_FORBIDDEN_COMPONENT_KEYS = {"install", "rollback", "repair", "handler", "python_handler", "callable", "module_path"}


class ManifestError(ValueError):
    pass


class VersionCompatibility(str, Enum):
    SATISFIED_TESTED = "satisfied_tested"
    SATISFIED_UNTESTED = "satisfied_untested"
    INCOMPATIBLE = "incompatible"
    UNPARSEABLE = "unparseable"


@dataclass(frozen=True, order=True)
class ParsedVersion:
    major: int
    minor: int
    patch: int = 0

    @property
    def tuple(self) -> tuple[int, int, int]:
        return (self.major, self.minor, self.patch)

    @classmethod
    def parse(cls, value: str | None) -> "ParsedVersion | None":
        if not value:
            return None
        match = _VERSION_RE.search(str(value))
        if not match:
            return None
        return cls(int(match.group(1)), int(match.group(2)), int(match.group(3) or 0))


@dataclass(frozen=True)
class VersionSpec:
    minimum_version: str | None = None
    maximum_version: str | None = None
    exact_version: str | None = None
    tested_ranges: tuple[str, ...] = ()
    known_incompatible: tuple[str, ...] = ()


@dataclass(frozen=True)
class ProbeSpec:
    kind: str
    args: Mapping[str, Any]


@dataclass(frozen=True)
class CapabilitySpec:
    id: str
    dependency_id: str
    display_name: str
    requirement: str
    lifecycle: tuple[str, ...]
    component_id: str | None
    probe: ProbeSpec


@dataclass(frozen=True)
class ExternalDependencySpec:
    id: str
    name: str
    reason: str | None
    version: VersionSpec


@dataclass(frozen=True)
class ComponentDependencySpec:
    id: str
    required: bool = True
    minimum_version: str | None = None


@dataclass(frozen=True)
class ArtifactSpec:
    id: str
    component_id: str
    path: str
    type: str
    required: bool
    ownership: str
    managed: bool
    source: str | None = None
    mode: str | None = None


@dataclass(frozen=True)
class HealthCheckSpec:
    id: str
    component_id: str
    check: str
    artifact_id: str | None
    cost: str
    side_effects: str
    timeout_ms: int
    contexts: tuple[str, ...]
    args: Mapping[str, Any]


@dataclass(frozen=True)
class ComponentSpec:
    id: str
    name: str
    component_version: str
    category: str
    stage: str
    description: str | None
    realmheart_dependencies: tuple[ComponentDependencySpec, ...]
    build_units: tuple[str, ...]
    repair_strategy_ids: tuple[str, ...]
    requires_installer_binding: bool = False


@dataclass(frozen=True)
class BuildUnitSpec:
    id: str
    cmake_target: str | None
    component_ids: tuple[str, ...]
    artifact_ids: tuple[str, ...]
    build_dependencies: tuple[str, ...]
    abi_sensitive_dependencies: tuple[str, ...]
    rebuild_on_dependency_change: tuple[str, ...]


@dataclass(frozen=True)
class ManifestRegistry:
    schema_version: int
    release_version: str
    digest: str
    source_files: tuple[str, ...]
    components: Mapping[str, ComponentSpec]
    dependencies: Mapping[str, ExternalDependencySpec]
    capabilities: Mapping[str, CapabilitySpec]
    artifacts: Mapping[str, ArtifactSpec]
    health_checks: Mapping[str, HealthCheckSpec]
    build_units: Mapping[str, BuildUnitSpec]
    component_order: tuple[str, ...]

    def capabilities_in_order(self) -> tuple[CapabilitySpec, ...]:
        # File order is canonical and deterministic because manifests are loaded
        # by sorted filename and TOML arrays preserve author order.
        return tuple(self.capabilities.values())

    def component_capabilities(self, component_id: str) -> tuple[CapabilitySpec, ...]:
        return tuple(spec for spec in self.capabilities.values() if spec.component_id == component_id)

    def progress_count(self) -> int:
        return len(self.component_order)


def _selector_matches(selector: str, version: ParsedVersion) -> bool:
    selector = selector.strip()
    if not selector:
        return False
    if selector.endswith(".x"):
        parts = selector[:-2].split(".")
        try:
            numbers = tuple(int(part) for part in parts)
        except ValueError:
            raise ManifestError(f"invalid version selector: {selector}") from None
        return version.tuple[:len(numbers)] == numbers
    for operator in (">=", "<=", ">", "<", "=="):
        if selector.startswith(operator):
            target = ParsedVersion.parse(selector[len(operator):].strip())
            if target is None:
                raise ManifestError(f"invalid version selector: {selector}")
            return {">=": version >= target, "<=": version <= target, ">": version > target, "<": version < target, "==": version == target}[operator]
    target = ParsedVersion.parse(selector)
    if target is None:
        raise ManifestError(f"invalid version selector: {selector}")
    return version == target


def classify_version(spec: VersionSpec, detected: str) -> VersionCompatibility:
    version = ParsedVersion.parse(detected)
    if version is None:
        return VersionCompatibility.UNPARSEABLE
    if any(_selector_matches(selector, version) for selector in spec.known_incompatible):
        return VersionCompatibility.INCOMPATIBLE
    exact = ParsedVersion.parse(spec.exact_version)
    if spec.exact_version and exact is None:
        raise ManifestError(f"invalid exact_version: {spec.exact_version}")
    if exact is not None and version != exact:
        return VersionCompatibility.INCOMPATIBLE
    minimum = ParsedVersion.parse(spec.minimum_version)
    if spec.minimum_version and minimum is None:
        raise ManifestError(f"invalid minimum_version: {spec.minimum_version}")
    if minimum is not None and version < minimum:
        return VersionCompatibility.INCOMPATIBLE
    maximum = ParsedVersion.parse(spec.maximum_version)
    if spec.maximum_version and maximum is None:
        raise ManifestError(f"invalid maximum_version: {spec.maximum_version}")
    if maximum is not None and version > maximum:
        return VersionCompatibility.INCOMPATIBLE
    if spec.tested_ranges and any(_selector_matches(selector, version) for selector in spec.tested_ranges):
        return VersionCompatibility.SATISFIED_TESTED
    return VersionCompatibility.SATISFIED_UNTESTED


def _expect_id(value: Any, *, what: str) -> str:
    if not isinstance(value, str) or not _ID_RE.fullmatch(value):
        raise ManifestError(f"invalid {what} id: {value!r}")
    return value


def _string_tuple(value: Any, *, field: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ManifestError(f"{field} must be an array of strings")
    return tuple(value)


def _bool(value: Any, default: bool = False) -> bool:
    return default if value is None else bool(value)


def _version_spec(raw: Mapping[str, Any]) -> VersionSpec:
    spec = VersionSpec(
        minimum_version=raw.get("minimum_version"),
        maximum_version=raw.get("maximum_version"),
        exact_version=raw.get("exact_version"),
        tested_ranges=_string_tuple(raw.get("tested_ranges"), field="tested_ranges"),
        known_incompatible=_string_tuple(raw.get("known_incompatible"), field="known_incompatible"),
    )
    for value in (spec.minimum_version, spec.maximum_version, spec.exact_version):
        if value is not None and ParsedVersion.parse(value) is None:
            raise ManifestError(f"invalid semantic version: {value!r}")
    for selector in (*spec.tested_ranges, *spec.known_incompatible):
        # Exercise parser against a harmless version to validate syntax; wildcard
        # selectors may simply evaluate false.
        _selector_matches(selector, ParsedVersion(0, 0, 0))
    return spec


def _safe_artifact_path(path: str) -> bool:
    if not path or "\x00" in path:
        return False
    normalized = path.replace("$HOME", "HOME").replace("$XDG_CONFIG_HOME", "XDG_CONFIG_HOME").replace("$XDG_STATE_HOME", "XDG_STATE_HOME").replace("$PREFIX", "PREFIX").replace("$LIBEXEC", "LIBEXEC").replace("$SYSCONF", "SYSCONF")
    return ".." not in PurePosixPath(normalized).parts


def _insert_unique(target: dict[str, Any], key: str, value: Any, *, what: str) -> None:
    if key in target:
        raise ManifestError(f"duplicate {what} id: {key}")
    target[key] = value


def load_manifest(components_dir: Path) -> ManifestRegistry:
    components_dir = Path(components_dir)
    files = tuple(sorted(components_dir.glob("*.toml")))
    if not files:
        raise ManifestError(f"no canonical component manifests found in {components_dir}")

    hasher = hashlib.sha256()
    schema_version: int | None = None
    release_version: str | None = None
    components: dict[str, ComponentSpec] = {}
    dependencies: dict[str, ExternalDependencySpec] = {}
    capabilities: dict[str, CapabilitySpec] = {}
    artifacts: dict[str, ArtifactSpec] = {}
    health_checks: dict[str, HealthCheckSpec] = {}
    build_units: dict[str, BuildUnitSpec] = {}

    for path in files:
        raw_bytes = path.read_bytes()
        hasher.update(path.name.encode("utf-8") + b"\0" + raw_bytes + b"\0")
        try:
            doc = tomllib.loads(raw_bytes.decode("utf-8"))
        except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
            raise ManifestError(f"cannot parse {path.name}: {exc}") from exc
        unknown_top = set(doc) - {"schema_version", "release_version", "components", "external_dependencies", "capabilities", "artifacts", "health_checks", "build_units"}
        if unknown_top:
            raise ManifestError(f"unsupported top-level manifest field(s) in {path.name}: {', '.join(sorted(unknown_top))}")
        current_schema = doc.get("schema_version")
        if not isinstance(current_schema, int):
            raise ManifestError(f"{path.name}: schema_version must be an integer")
        if current_schema > SUPPORTED_SCHEMA_VERSION:
            raise ManifestError(f"{path.name}: manifest schema {current_schema} is newer than supported schema {SUPPORTED_SCHEMA_VERSION}")
        if current_schema != SUPPORTED_SCHEMA_VERSION:
            raise ManifestError(f"{path.name}: unsupported manifest schema {current_schema}")
        current_release = doc.get("release_version")
        if not isinstance(current_release, str) or ParsedVersion.parse(current_release) is None:
            raise ManifestError(f"{path.name}: invalid release_version")
        if schema_version is None:
            schema_version, release_version = current_schema, current_release
        elif current_schema != schema_version or current_release != release_version:
            raise ManifestError(f"{path.name}: manifest schema/release identity disagrees with manifest set")

        for raw in doc.get("components", []):
            if not isinstance(raw, dict):
                raise ManifestError(f"{path.name}: component record must be a table")
            forbidden = _FORBIDDEN_COMPONENT_KEYS & set(raw)
            if forbidden:
                raise ManifestError(f"{path.name}: tool-specific component field(s) are forbidden: {', '.join(sorted(forbidden))}")
            cid = _expect_id(raw.get("id"), what="component")
            category = raw.get("category")
            stage = raw.get("stage")
            if category not in _ALLOWED_CATEGORIES:
                raise ManifestError(f"component {cid}: unrecognized category {category!r}")
            if stage not in _ALLOWED_STAGES:
                raise ManifestError(f"component {cid}: unrecognized stage {stage!r}")
            version = raw.get("component_version")
            if version == "release":
                version = current_release
            if not isinstance(version, str) or ParsedVersion.parse(version) is None:
                raise ManifestError(f"component {cid}: invalid component_version")
            deps: list[ComponentDependencySpec] = []
            for dep in raw.get("realmheart_dependencies", []):
                if not isinstance(dep, dict):
                    raise ManifestError(f"component {cid}: dependency must be a table")
                minimum_version = dep.get("minimum_version")
                if minimum_version is not None and ParsedVersion.parse(str(minimum_version)) is None:
                    raise ManifestError(f"component {cid}: invalid dependency minimum_version {minimum_version!r}")
                deps.append(ComponentDependencySpec(_expect_id(dep.get("id"), what="component dependency"), _bool(dep.get("required"), True), minimum_version))
            component = ComponentSpec(
                id=cid,
                name=str(raw.get("name") or cid),
                component_version=version,
                category=category,
                stage=stage,
                description=raw.get("description"),
                realmheart_dependencies=tuple(deps),
                build_units=_string_tuple(raw.get("build_units"), field=f"component {cid} build_units"),
                repair_strategy_ids=_string_tuple(raw.get("repair_strategy_ids"), field=f"component {cid} repair_strategy_ids"),
                requires_installer_binding=_bool(raw.get("requires_installer_binding"), False),
            )
            _insert_unique(components, cid, component, what="component")

        for raw in doc.get("external_dependencies", []):
            did = _expect_id(raw.get("id"), what="external dependency")
            _insert_unique(dependencies, did, ExternalDependencySpec(did, str(raw.get("name") or did), raw.get("reason"), _version_spec(raw)), what="external dependency")

        for raw in doc.get("capabilities", []):
            capid = _expect_id(raw.get("id"), what="capability")
            dependency_id = _expect_id(raw.get("dependency_id"), what="capability dependency")
            requirement = raw.get("requirement")
            lifecycle = _string_tuple(raw.get("lifecycle"), field=f"capability {capid} lifecycle")
            if requirement not in _ALLOWED_REQUIREMENTS:
                raise ManifestError(f"capability {capid}: invalid requirement {requirement!r}")
            if not lifecycle or any(item not in _ALLOWED_LIFECYCLES for item in lifecycle):
                raise ManifestError(f"capability {capid}: invalid lifecycle {lifecycle!r}")
            component_id = raw.get("component_id")
            if component_id is not None:
                component_id = _expect_id(component_id, what="capability component")
            probe_raw = raw.get("probe")
            if not isinstance(probe_raw, dict):
                raise ManifestError(f"capability {capid}: probe table is required")
            kind = probe_raw.get("kind")
            if kind not in _ALLOWED_PROBES:
                raise ManifestError(f"capability {capid}: unsupported probe kind {kind!r}")
            args = {key: value for key, value in probe_raw.items() if key != "kind"}
            required_args = {
                "executable": ("executable",), "pkg_config": ("module",), "tesseract_language": ("language",),
                "portal_unit": ("unit",), "any_command": ("commands",), "command_group": ("commands",),
            }.get(kind, ())
            if any(not args.get(key) for key in required_args):
                raise ManifestError(f"capability {capid}: probe {kind} missing required argument(s): {', '.join(required_args)}")
            cap = CapabilitySpec(capid, dependency_id, str(raw.get("display_name") or capid), requirement, lifecycle, component_id, ProbeSpec(kind, args))
            _insert_unique(capabilities, capid, cap, what="capability")

        for raw in doc.get("artifacts", []):
            aid = _expect_id(raw.get("id"), what="artifact")
            cid = _expect_id(raw.get("component_id"), what="artifact component")
            atype = raw.get("type")
            ownership = raw.get("ownership", "release")
            path_text = raw.get("path")
            if atype not in _ALLOWED_ARTIFACT_TYPES:
                raise ManifestError(f"artifact {aid}: invalid type {atype!r}")
            if ownership not in _ALLOWED_OWNERSHIP:
                raise ManifestError(f"artifact {aid}: invalid ownership {ownership!r}")
            if not isinstance(path_text, str) or not _safe_artifact_path(path_text):
                raise ManifestError(f"artifact {aid}: unsafe path {path_text!r}")
            source = raw.get("source")
            if source is not None:
                if not isinstance(source, str) or Path(source).is_absolute() or ".." in PurePosixPath(source).parts:
                    raise ManifestError(f"artifact {aid}: unsafe source path {source!r}")
            mode = raw.get("mode")
            if mode is not None:
                if not isinstance(mode, str) or len(mode) != 4 or any(ch not in "01234567" for ch in mode):
                    raise ManifestError(f"artifact {aid}: invalid mode {mode!r}; expected four octal digits")
            artifact = ArtifactSpec(aid, cid, path_text, atype, _bool(raw.get("required"), True), ownership, _bool(raw.get("managed"), True), source, mode)
            _insert_unique(artifacts, aid, artifact, what="artifact")

        for raw in doc.get("health_checks", []):
            hid = _expect_id(raw.get("id"), what="health check")
            cid = _expect_id(raw.get("component_id"), what="health check component")
            check = raw.get("check")
            if check not in _ALLOWED_HEALTH_CHECKS:
                raise ManifestError(f"health check {hid}: unsupported check {check!r}")
            cost = raw.get("cost", "cheap")
            side_effects = raw.get("side_effects", "none")
            if cost not in _ALLOWED_COST or side_effects not in _ALLOWED_SIDE_EFFECTS:
                raise ManifestError(f"health check {hid}: invalid cost/side_effects")
            timeout = raw.get("timeout_ms", 1000)
            if not isinstance(timeout, int) or timeout <= 0:
                raise ManifestError(f"health check {hid}: timeout_ms must be a positive integer")
            health = HealthCheckSpec(hid, cid, check, raw.get("artifact_id"), cost, side_effects, timeout, _string_tuple(raw.get("contexts"), field=f"health check {hid} contexts"), dict(raw.get("args") or {}))
            _insert_unique(health_checks, hid, health, what="health check")

        for raw in doc.get("build_units", []):
            bid = _expect_id(raw.get("id"), what="build unit")
            unit = BuildUnitSpec(
                bid, raw.get("cmake_target"),
                _string_tuple(raw.get("component_ids"), field=f"build unit {bid} component_ids"),
                _string_tuple(raw.get("artifact_ids"), field=f"build unit {bid} artifact_ids"),
                _string_tuple(raw.get("build_dependencies"), field=f"build unit {bid} build_dependencies"),
                _string_tuple(raw.get("abi_sensitive_dependencies"), field=f"build unit {bid} abi_sensitive_dependencies"),
                _string_tuple(raw.get("rebuild_on_dependency_change"), field=f"build unit {bid} rebuild_on_dependency_change"),
            )
            _insert_unique(build_units, bid, unit, what="build unit")

    assert schema_version is not None and release_version is not None

    for component in components.values():
        for dep in component.realmheart_dependencies:
            if dep.id not in components:
                raise ManifestError(f"component {component.id}: unresolved Realmheart dependency {dep.id}")
            if dep.minimum_version is not None:
                target_version = ParsedVersion.parse(components[dep.id].component_version)
                minimum = ParsedVersion.parse(dep.minimum_version)
                if target_version is None or minimum is None or target_version < minimum:
                    raise ManifestError(
                        f"component {component.id}: dependency {dep.id} does not satisfy minimum component version {dep.minimum_version}"
                    )
        for build_id in component.build_units:
            if build_id not in build_units:
                raise ManifestError(f"component {component.id}: unresolved build unit {build_id}")
    for cap in capabilities.values():
        if cap.dependency_id not in dependencies:
            raise ManifestError(f"capability {cap.id}: unresolved external dependency {cap.dependency_id}")
        if cap.component_id is not None and cap.component_id not in components:
            raise ManifestError(f"capability {cap.id}: unresolved component {cap.component_id}")
    for artifact in artifacts.values():
        if artifact.component_id not in components:
            raise ManifestError(f"artifact {artifact.id}: unresolved component {artifact.component_id}")
    for check in health_checks.values():
        if check.component_id not in components:
            raise ManifestError(f"health check {check.id}: unresolved component {check.component_id}")
        if check.artifact_id is not None and check.artifact_id not in artifacts:
            raise ManifestError(f"health check {check.id}: unresolved artifact {check.artifact_id}")
    for unit in build_units.values():
        for cid in unit.component_ids:
            if cid not in components:
                raise ManifestError(f"build unit {unit.id}: unresolved component {cid}")
        for aid in unit.artifact_ids:
            if aid not in artifacts:
                raise ManifestError(f"build unit {unit.id}: unresolved artifact {aid}")
        for did in (*unit.build_dependencies, *unit.abi_sensitive_dependencies, *unit.rebuild_on_dependency_change):
            if did not in dependencies:
                raise ManifestError(f"build unit {unit.id}: unresolved dependency {did}")

    # Deterministic Kahn topological order.
    incoming = {cid: {dep.id for dep in component.realmheart_dependencies} for cid, component in components.items()}
    order: list[str] = []
    ready = sorted(cid for cid, deps in incoming.items() if not deps)
    while ready:
        cid = ready.pop(0)
        order.append(cid)
        for other in sorted(incoming):
            if cid in incoming[other]:
                incoming[other].remove(cid)
                if not incoming[other] and other not in order and other not in ready:
                    ready.append(other)
                    ready.sort()
    if len(order) != len(components):
        cyclic = sorted(cid for cid, deps in incoming.items() if deps)
        raise ManifestError("Realmheart component dependency cycle detected: " + ", ".join(cyclic))

    return ManifestRegistry(
        schema_version, release_version, hasher.hexdigest(), tuple(path.name for path in files),
        MappingProxyType(dict(components)), MappingProxyType(dict(dependencies)),
        MappingProxyType(dict(capabilities)), MappingProxyType(dict(artifacts)),
        MappingProxyType(dict(health_checks)), MappingProxyType(dict(build_units)), tuple(order),
    )
