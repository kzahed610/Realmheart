"""Read-only Phase-17 uninstall planner.

The planner deliberately treats both the installed-state receipt and historical
transaction data as untrusted input.  Receipt artifact paths are authorized only
when the artifact ID still exists in the canonical manifest *and* the resolved
canonical path exactly matches the receipt path.
"""
from __future__ import annotations

import json
import os
import stat
from pathlib import Path
from typing import Any

from realmheart_maintenance.manifest import ManifestRegistry, load_manifest

from ..context import XdgPaths
from ..filesystem.backup import validate_backup_snapshot
from ..filesystem.compare import fingerprint_path
from ..transaction.journal import read_journal
from ..models import OperationState
from .models import (
    BaselineEntry,
    ConfigComparison,
    DifferenceEntry,
    DifferenceKind,
    FootprintAction,
    FootprintEntry,
    PackageCleanupCandidate,
    ServiceState,
    UninstallPlan,
)


_UNINSTALL_SCHEMA_VERSION = 1
_CONFIG_BASELINE_PREFIXES = ("config.",)
_PRIVILEGED_OWNERSHIP = {"release", "system"}
_SHARED_ARTIFACTS_NEVER_RECURSIVE = {"realmheart.config"}


class UninstallPlanner:
    def __init__(
        self,
        *,
        paths: XdgPaths,
        source_root: Path,
        transaction_id: str,
        registry: ManifestRegistry | None = None,
        prefix: Path = Path("/usr/local"),
        sysconf: Path = Path("/etc"),
    ) -> None:
        self.paths = paths
        self.source_root = Path(source_root)
        self.transaction_id = transaction_id
        self.registry = registry or load_manifest(self.source_root / "components")
        self.prefix = Path(prefix)
        self.sysconf = Path(sysconf)

    def build(self) -> UninstallPlan:
        warnings: list[str] = []
        blockers: list[str] = []
        receipt_path = self.paths.realmheart_state / "installed-state.json"
        receipt = self._load_receipt(receipt_path, blockers)
        managed = receipt is not None
        installed_version = _string(receipt.get("realmheart_version")) if receipt else None
        install_txid = _string(receipt.get("transaction_id")) if receipt else None

        baseline_entries, baseline_available, baseline_valid = self._load_baseline(blockers)
        baseline_by_target = {entry.target: entry for entry in baseline_entries}

        last_managed: dict[str, str] = {}
        service_states: tuple[ServiceState, ...] = ()
        package_candidates: tuple[PackageCleanupCandidate, ...] = ()
        if install_txid:
            if not _safe_transaction_id(install_txid):
                blockers.append("installed-state receipt contains an unsafe installation transaction ID")
            else:
                transaction_dir = self.paths.transactions / install_txid
                last_managed = _load_last_managed_fingerprints(transaction_dir / "journal.jsonl", warnings)
                service_states = _load_service_states(transaction_dir / "journal.jsonl", warnings)
                package_candidates = _load_package_candidates(transaction_dir / "transaction.json", warnings)
        elif receipt:
            warnings.append("installed-state receipt has no transaction_id; historical service/package provenance is unavailable")

        footprint: list[FootprintEntry] = []
        if receipt:
            raw_artifacts = receipt.get("artifacts")
            if not isinstance(raw_artifacts, dict):
                blockers.append("installed-state receipt has no valid artifacts mapping")
            else:
                footprint.extend(self._artifact_footprint(raw_artifacts, baseline_by_target, last_managed, warnings, blockers))

            receipt_manifest = _string(receipt.get("manifest_set_sha256"))
            if receipt_manifest and receipt_manifest != self.registry.digest:
                warnings.append(
                    "installed receipt manifest digest differs from the source checkout; artifact IDs/paths were re-authorized against the current canonical manifest before planning removal"
                )

        shared_seeds = tuple(
            entry for entry in baseline_entries
            if entry.label.startswith("config.realmheart.seed.")
        )
        shared_root = self.paths.config_home / "realmheart"
        existing_targets = {item.target for item in footprint}
        for entry in shared_seeds:
            target = Path(entry.target)
            if not _within(Path(os.path.abspath(target)), Path(os.path.abspath(shared_root))):
                blockers.append(f"shared Realmheart seed baseline target escapes the approved namespace: {entry.target}")
                continue
            if entry.existed or entry.target in existing_targets:
                continue
            current_exists = target.exists() or target.is_symlink()
            current_fp = fingerprint_path(target)
            managed_fp = last_managed.get(entry.target)
            diverged = bool(current_exists and managed_fp and current_fp != managed_fp)
            footprint.append(FootprintEntry(
                artifact_id=entry.label,
                target=entry.target,
                artifact_type=entry.source_type or "file",
                ownership="shared",
                current_exists=current_exists,
                baseline_existed=False,
                baseline_backup_path=None,
                baseline_source_type=None,
                baseline_source_mode=None,
                baseline_source_uid=None,
                baseline_source_gid=None,
                last_managed_fingerprint=managed_fp,
                current_fingerprint=current_fp,
                diverged=diverged,
                keep_current_action=FootprintAction.PRESERVE_CONFLICT if diverged else FootprintAction.REMOVE,
                reason=(
                    "user-edited shared seed differs from the installer-owned post-state; preserve it"
                    if diverged else
                    "shared seed was absent before Realmheart and is safe to remove by exact path"
                ),
                privileged=False,
            ))

        comparisons = self._build_config_comparisons(baseline_entries)

        preserved = (
            str(self.paths.realmheart_state / "events.db"),
            str(self.paths.realmheart_state / "theme-palette.tsv"),
        )
        if not baseline_available:
            warnings.append("permanent pre-Realmheart baseline is unavailable; baseline restoration will not be offered")

        return UninstallPlan(
            schema_version=_UNINSTALL_SCHEMA_VERSION,
            transaction_id=self.transaction_id,
            installed_version=installed_version,
            install_transaction_id=install_txid,
            receipt_path=str(receipt_path),
            managed_install=managed,
            baseline_path=str(self.paths.baseline_backup),
            baseline_available=baseline_available,
            baseline_valid=baseline_valid,
            baseline_entries=baseline_entries,
            footprint=tuple(sorted(footprint, key=lambda item: (item.privileged, item.target))),
            comparisons=comparisons,
            service_states=service_states,
            package_cleanup_candidates=package_candidates,
            shared_seed_entries=shared_seeds,
            preserved_paths=preserved,
            warnings=tuple(warnings),
            blockers=tuple(dict.fromkeys(blockers)),
        )

    def _load_receipt(self, path: Path, blockers: list[str]) -> dict[str, Any] | None:
        if not path.is_file() or path.is_symlink():
            blockers.append("no authoritative managed installed-state receipt exists; refusing to guess an uninstall footprint")
            return None
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            blockers.append(f"installed-state receipt is unreadable/corrupt: {exc}")
            return None
        if not isinstance(payload, dict):
            blockers.append("installed-state receipt is not a JSON object")
            return None
        if payload.get("disposition") != "kept":
            blockers.append(f"installed-state receipt disposition is not 'kept': {payload.get('disposition')!r}")
        return payload

    def _load_baseline(self, blockers: list[str]) -> tuple[tuple[BaselineEntry, ...], bool, bool]:
        root = self.paths.baseline_backup
        if not (root.exists() or root.is_symlink()):
            return (), False, False
        validation = validate_backup_snapshot(root)
        if not validation.valid:
            blockers.append("permanent baseline is invalid: " + "; ".join(validation.errors))
            return (), True, False
        try:
            payload = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:  # validation should already catch this
            blockers.append(f"permanent baseline manifest became unreadable: {exc}")
            return (), True, False
        entries: list[BaselineEntry] = []
        for raw in payload.get("sources", []):
            if not isinstance(raw, dict):
                continue
            label = raw.get("label")
            target = raw.get("source")
            existed = raw.get("existed")
            rel = raw.get("backup_relative_path")
            fp = raw.get("backup_fingerprint")
            if not isinstance(label, str) or not isinstance(target, str) or not isinstance(existed, bool):
                blockers.append("permanent baseline contains a malformed source record")
                continue
            backup = str(root / rel) if existed and isinstance(rel, str) else None
            entries.append(BaselineEntry(
                label, target, existed, backup, fp if isinstance(fp, str) else None,
                raw.get("source_type") if isinstance(raw.get("source_type"), str) else None,
                raw.get("source_mode") if isinstance(raw.get("source_mode"), str) else None,
                raw.get("source_uid") if isinstance(raw.get("source_uid"), int) and not isinstance(raw.get("source_uid"), bool) else None,
                raw.get("source_gid") if isinstance(raw.get("source_gid"), int) and not isinstance(raw.get("source_gid"), bool) else None,
            ))
        return tuple(entries), True, True

    def _artifact_footprint(
        self,
        raw_artifacts: dict[str, Any],
        baseline_by_target: dict[str, BaselineEntry],
        last_managed: dict[str, str],
        warnings: list[str],
        blockers: list[str],
    ) -> list[FootprintEntry]:
        result: list[FootprintEntry] = []
        for artifact_id, raw in sorted(raw_artifacts.items()):
            if not isinstance(artifact_id, str) or not isinstance(raw, dict):
                blockers.append("installed-state receipt contains malformed artifact data")
                continue
            canonical = self.registry.artifacts.get(artifact_id)
            if canonical is None:
                blockers.append(
                    f"receipt artifact {artifact_id} is unknown to the current canonical manifest; "
                    "refusing to retire the managed receipt while its ownership/path cannot be re-authorized"
                )
                continue
            receipt_type = raw.get("type")
            receipt_ownership = raw.get("ownership")
            if receipt_type is not None and receipt_type != canonical.type:
                blockers.append(
                    f"receipt artifact type conflicts with the canonical manifest: {artifact_id} -> "
                    f"{receipt_type!r} (expected {canonical.type!r})"
                )
                continue
            if receipt_ownership is not None and receipt_ownership != canonical.ownership:
                blockers.append(
                    f"receipt artifact ownership conflicts with the canonical manifest: {artifact_id} -> "
                    f"{receipt_ownership!r} (expected {canonical.ownership!r})"
                )
                continue
            receipt_path = raw.get("path")
            expected_path = self._expand_path(canonical.path)
            if not isinstance(receipt_path, str) or receipt_path != expected_path:
                blockers.append(
                    f"receipt artifact path is not authorized by the canonical manifest: {artifact_id} -> {receipt_path!r} (expected {expected_path!r})"
                )
                continue
            if artifact_id in _SHARED_ARTIFACTS_NEVER_RECURSIVE:
                # The containing Realmheart namespace has user data/state.  Its
                # individually seeded/owned children are handled separately.
                continue

            target = Path(receipt_path)
            current_exists = target.exists() or target.is_symlink()
            current_fp = fingerprint_path(target)
            receipt_fp = raw.get("immutable_fingerprint") if isinstance(raw.get("immutable_fingerprint"), str) else None
            managed_fp = last_managed.get(receipt_path) or receipt_fp
            missing_mutable_provenance = bool(
                current_exists
                and managed_fp is None
                and canonical.ownership in {"user", "shared"}
                and canonical.type != "generated"
            )
            diverged = bool(current_exists and managed_fp and current_fp != managed_fp)
            baseline = baseline_by_target.get(receipt_path)
            privileged = canonical.ownership in _PRIVILEGED_OWNERSHIP or _is_privileged_target(target, self.prefix, self.sysconf)
            if privileged and baseline and baseline.existed:
                if baseline.source_type != "file" or baseline.source_mode is None or baseline.source_uid is None or baseline.source_gid is None:
                    blockers.append(
                        f"privileged baseline metadata is insufficient for exact restoration of {artifact_id}; refusing to guess ownership/mode"
                    )

            if artifact_id == "hypr.tree":
                action = FootprintAction.PRESERVE
                reason = "current Hyprland tree is configuration; keep-current leaves it intact and restore-baseline handles it through the staged full-tree engine"
            elif canonical.type == "generated":
                # Generated terminal outputs are expected to change whenever the
                # palette/wallpaper changes.  Their install-time fingerprint is
                # therefore not an ownership boundary; exact canonical path +
                # plan-time compare-before-write is the correct uninstall guard.
                action = FootprintAction.REMOVE
                reason = "Realmheart-owned generated output; remove by exact canonical path even when its generated contents changed after installation"
            elif missing_mutable_provenance:
                action = FootprintAction.PRESERVE_CONFLICT
                reason = "historical last-managed fingerprint is unavailable for this mutable user-owned path; keep-current refuses to guess that current bytes are still installer-owned"
            elif diverged:
                action = FootprintAction.PRESERVE_CONFLICT
                reason = "current path differs from the last installer-owned fingerprint; keep-current refuses destructive cleanup"
            elif baseline and baseline.existed:
                action = FootprintAction.RESTORE_PREIMAGE
                reason = "Realmheart replaced a pre-existing same-name path; restore its permanent-baseline preimage"
            else:
                action = FootprintAction.REMOVE
                reason = "Realmheart-managed path did not pre-exist in the permanent baseline"

            result.append(FootprintEntry(
                artifact_id=artifact_id,
                target=receipt_path,
                artifact_type=canonical.type,
                ownership=canonical.ownership,
                current_exists=current_exists,
                baseline_existed=baseline.existed if baseline else None,
                baseline_backup_path=baseline.backup_path if baseline else None,
                baseline_source_type=baseline.source_type if baseline else None,
                baseline_source_mode=baseline.source_mode if baseline else None,
                baseline_source_uid=baseline.source_uid if baseline else None,
                baseline_source_gid=baseline.source_gid if baseline else None,
                last_managed_fingerprint=managed_fp,
                current_fingerprint=current_fp,
                diverged=diverged,
                keep_current_action=action,
                reason=reason,
                privileged=privileged,
            ))
        return result

    def _build_config_comparisons(self, entries: tuple[BaselineEntry, ...]) -> tuple[ConfigComparison, ...]:
        comparisons: list[ConfigComparison] = []
        for entry in entries:
            if not entry.label.startswith(_CONFIG_BASELINE_PREFIXES):
                continue
            # Shared/owned drop-ins are represented too, but the user-facing
            # compare remains finite and content-free: paths + change kinds only.
            current = Path(entry.target)
            baseline = Path(entry.backup_path) if entry.backup_path else None
            current_fp = fingerprint_path(current)
            differences = _compare_path_shapes(current, baseline, entry.target)
            comparisons.append(ConfigComparison(
                target=entry.target,
                baseline_label=entry.label,
                changed=bool(differences),
                current_fingerprint=current_fp,
                baseline_fingerprint=entry.baseline_fingerprint,
                differences=differences,
            ))
        return tuple(sorted(comparisons, key=lambda item: item.target))

    def _expand_path(self, value: str) -> str:
        replacements = {
            "$PREFIX": str(self.prefix),
            "$LIBEXEC": str(self.prefix / "libexec"),
            "$SYSCONF": str(self.sysconf),
            "$HOME": str(self.paths.home),
            "$XDG_CONFIG_HOME": str(self.paths.config_home),
            "$XDG_STATE_HOME": str(self.paths.state_home),
        }
        rendered = value
        for token in sorted(replacements, key=len, reverse=True):
            rendered = rendered.replace(token, replacements[token])
        return rendered


def _load_last_managed_fingerprints(path: Path, warnings: list[str]) -> dict[str, str]:
    if not path.is_file():
        warnings.append("installation journal is unavailable; mutable user-owned artifact divergence cannot be proven for every path")
        return {}
    try:
        events = read_journal(path)
    except Exception as exc:
        warnings.append(f"installation journal could not be read for divergence provenance: {exc}")
        return {}
    completed = {event.operation_id for event in events if event.state is OperationState.COMPLETED}
    result: dict[str, str] = {}
    for event in events:
        if event.operation_id not in completed or not event.data:
            continue
        if event.state is OperationState.INTENT and event.kind == "write_file":
            fp = event.data.get("expected_after_fingerprint")
            if event.target and isinstance(fp, str):
                result[event.target] = fp
        elif event.state is OperationState.INTENT and event.kind == "move_path":
            destination = event.data.get("destination")
            fp = event.data.get("before_fingerprint")
            if isinstance(destination, str) and isinstance(fp, str):
                result[destination] = fp
        elif event.state is OperationState.COMPLETED and event.kind == "privileged_replace":
            fp = event.data.get("after_fingerprint")
            if event.target and isinstance(fp, str):
                result[event.target] = fp
    return result


def _load_service_states(path: Path, warnings: list[str]) -> tuple[ServiceState, ...]:
    if not path.is_file():
        return ()
    try:
        events = read_journal(path)
    except Exception as exc:
        warnings.append(f"installation journal could not be read for previous user-service state: {exc}")
        return ()
    completed = {event.operation_id for event in events if event.state is OperationState.COMPLETED}
    states: dict[str, ServiceState] = {}
    for event in events:
        if event.operation_id not in completed or event.state is not OperationState.INTENT or event.kind != "systemd_user_services" or not event.data:
            continue
        before = event.data.get("before")
        if not isinstance(before, dict):
            continue
        for name, raw in before.items():
            if not isinstance(name, str) or not isinstance(raw, dict):
                continue
            enabled = raw.get("enabled") if isinstance(raw.get("enabled"), bool) else None
            active = raw.get("active") if isinstance(raw.get("active"), bool) else None
            states[name] = ServiceState(name, enabled, active)
    return tuple(states[key] for key in sorted(states))


def _load_package_candidates(path: Path, warnings: list[str]) -> tuple[PackageCleanupCandidate, ...]:
    if not path.is_file():
        return ()
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        warnings.append(f"installation transaction package provenance is unreadable: {exc}")
        return ()
    metadata = payload.get("metadata") if isinstance(payload, dict) else None
    package_install = metadata.get("package_install") if isinstance(metadata, dict) else None
    provenance = package_install.get("provenance") if isinstance(package_install, dict) else None
    if not isinstance(provenance, list):
        return ()
    result: list[PackageCleanupCandidate] = []
    for raw in provenance:
        if not isinstance(raw, dict) or raw.get("installed_by_transaction") is not True:
            continue
        package = raw.get("package")
        if not isinstance(package, str) or not package:
            continue
        required_by = raw.get("required_by")
        result.append(PackageCleanupCandidate(
            package,
            raw.get("version_after") if isinstance(raw.get("version_after"), str) else None,
            tuple(item for item in required_by if isinstance(item, str)) if isinstance(required_by, list) else (),
        ))
    return tuple(sorted(result, key=lambda item: item.package))


def _compare_path_shapes(current: Path, baseline: Path | None, target_text: str) -> tuple[DifferenceEntry, ...]:
    current_exists = current.exists() or current.is_symlink()
    baseline_exists = baseline is not None and (baseline.exists() or baseline.is_symlink())
    if not current_exists and not baseline_exists:
        return ()
    if current_exists and not baseline_exists:
        return _inventory_as_changes(current, target_text, DifferenceKind.ADDED)
    if baseline_exists and not current_exists:
        assert baseline is not None
        return _inventory_as_changes(baseline, target_text, DifferenceKind.REMOVED)
    assert baseline is not None

    current_inventory = _inventory(current)
    baseline_inventory = _inventory(baseline)
    keys = sorted(set(current_inventory) | set(baseline_inventory))
    differences: list[DifferenceEntry] = []
    for relative in keys:
        left = baseline_inventory.get(relative)
        right = current_inventory.get(relative)
        display = relative or "."
        if left is None:
            differences.append(DifferenceEntry(target_text, display, DifferenceKind.ADDED))
        elif right is None:
            differences.append(DifferenceEntry(target_text, display, DifferenceKind.REMOVED))
        elif left[0] != right[0]:
            differences.append(DifferenceEntry(target_text, display, DifferenceKind.TYPE_CHANGED))
        elif left[1] != right[1] and left[0] != "directory":
            differences.append(DifferenceEntry(target_text, display, DifferenceKind.CHANGED))
    return tuple(differences)


def _inventory(root: Path) -> dict[str, tuple[str, str]]:
    st = root.lstat()
    root_type = _fs_type(st)
    values = {"": (root_type, fingerprint_path(root))}
    if root_type != "directory":
        return values
    stack = [(root, Path())]
    while stack:
        directory, prefix = stack.pop()
        with os.scandir(directory) as iterator:
            entries = sorted(iterator, key=lambda item: os.fsencode(item.name), reverse=True)
        for entry in entries:
            relative = prefix / entry.name
            child = Path(entry.path)
            child_st = entry.stat(follow_symlinks=False)
            kind = _fs_type(child_st)
            values[relative.as_posix()] = (kind, fingerprint_path(child))
            if kind == "directory":
                stack.append((child, relative))
    return values


def _inventory_as_changes(root: Path, target: str, kind: DifferenceKind) -> tuple[DifferenceEntry, ...]:
    return tuple(DifferenceEntry(target, relative or ".", kind) for relative in sorted(_inventory(root)))


def _fs_type(st: os.stat_result) -> str:
    if stat.S_ISLNK(st.st_mode):
        return "symlink"
    if stat.S_ISREG(st.st_mode):
        return "file"
    if stat.S_ISDIR(st.st_mode):
        return "directory"
    return "special"


def _is_privileged_target(path: Path, prefix: Path, sysconf: Path) -> bool:
    absolute = Path(os.path.abspath(path))
    return _within(absolute, Path(os.path.abspath(prefix))) or _within(absolute, Path(os.path.abspath(sysconf)))


def _within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _safe_transaction_id(value: str) -> bool:
    return Path(value).name == value and value.startswith("RH-") and all(ch.isalnum() or ch in "-_" for ch in value)


def _string(value: Any) -> str | None:
    return value if isinstance(value, str) and value else None
