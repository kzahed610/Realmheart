"""Permanent baseline and generic snapshot backup primitives."""

from __future__ import annotations

import json
import os
import shutil
import stat
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Mapping

from ..constants import STATE_SCHEMA_VERSION
from ..durability import fsync_directory
from ..errors import BackupError, BaselineInvalidError
from .compare import fingerprint_path


@dataclass(frozen=True)
class BackupSourceRecord:
    label: str
    source: str
    existed: bool
    source_fingerprint: str
    backup_relative_path: str | None
    backup_fingerprint: str | None
    source_type: str | None = None
    source_mode: str | None = None
    source_uid: int | None = None
    source_gid: int | None = None


@dataclass(frozen=True)
class BackupValidation:
    valid: bool
    errors: tuple[str, ...]


def ensure_permanent_baseline(
    baseline_dir: Path,
    sources: Mapping[str, Path],
    *,
    installer_version: str,
    target_realmheart_version: str,
    transaction_id: str,
) -> Path:
    """Create the immutable first-install baseline or validate the existing one."""

    baseline_dir = Path(baseline_dir)
    if baseline_dir.exists() or baseline_dir.is_symlink():
        validation = validate_backup_snapshot(baseline_dir)
        if not validation.valid:
            raise BaselineInvalidError(
                "Existing permanent baseline is invalid; refusing to overwrite it: "
                + "; ".join(validation.errors)
            )
        return baseline_dir

    return create_backup_snapshot(
        baseline_dir,
        sources,
        snapshot_kind="permanent_baseline",
        installer_version=installer_version,
        target_realmheart_version=target_realmheart_version,
        transaction_id=transaction_id,
    )


def create_backup_snapshot(
    destination: Path,
    sources: Mapping[str, Path],
    *,
    snapshot_kind: str,
    installer_version: str,
    target_realmheart_version: str,
    transaction_id: str,
) -> Path:
    destination = Path(destination)
    if destination.exists() or destination.is_symlink():
        raise BackupError(
            f"Backup destination already exists: {destination}",
            code="RH_BACKUP_DESTINATION_EXISTS",
        )
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    staging = destination.parent / f".{destination.name}.staging-{transaction_id}-{uuid.uuid4().hex[:8]}"
    if staging.exists() or staging.is_symlink():
        raise BackupError(f"Unexpected backup staging collision: {staging}")

    content_root = staging / "content"
    content_root.mkdir(parents=True, mode=0o700)
    records: list[BackupSourceRecord] = []

    try:
        for label, source_raw in sorted(sources.items()):
            _validate_label(label)
            source = Path(source_raw)
            existed = source.exists() or source.is_symlink()
            before = fingerprint_path(source)
            backup_rel: str | None = None
            backup_fingerprint: str | None = None

            source_type: str | None = None
            source_mode: str | None = None
            source_uid: int | None = None
            source_gid: int | None = None
            if existed:
                source_stat = source.lstat()
                source_type = _filesystem_type(source_stat.st_mode)
                source_mode = f"{stat.S_IMODE(source_stat.st_mode):04o}"
                source_uid = source_stat.st_uid
                source_gid = source_stat.st_gid
                backup_path = content_root / label
                _copy_path_symlink_safe(source, backup_path)
                after_source = fingerprint_path(source)
                if after_source != before:
                    raise BackupError(
                        f"Source changed while baseline was being captured: {source}",
                        code="RH_BACKUP_SOURCE_DRIFT",
                        details={"label": label, "source": str(source)},
                    )
                backup_rel = str(Path("content") / label)
                backup_fingerprint = fingerprint_path(backup_path)
                if backup_fingerprint != before:
                    raise BackupError(
                        f"Backup verification mismatch for {source}",
                        code="RH_BACKUP_VERIFY_MISMATCH",
                        details={"label": label, "source": str(source)},
                    )

            records.append(
                BackupSourceRecord(
                    label=label,
                    source=str(source),
                    existed=existed,
                    source_fingerprint=before,
                    backup_relative_path=backup_rel,
                    backup_fingerprint=backup_fingerprint,
                    source_type=source_type,
                    source_mode=source_mode,
                    source_uid=source_uid,
                    source_gid=source_gid,
                )
            )

        manifest = {
            "schema_version": STATE_SCHEMA_VERSION,
            "snapshot_kind": snapshot_kind,
            "transaction_id": transaction_id,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "installer_version": installer_version,
            "target_realmheart_version": target_realmheart_version,
            "sources": [record.__dict__ for record in records],
        }
        _durable_json_write(staging / "manifest.json", manifest)

        validation = validate_backup_snapshot(staging)
        if not validation.valid:
            raise BackupError(
                "Staged backup failed self-validation: " + "; ".join(validation.errors),
                code="RH_BACKUP_SELF_VALIDATION_FAILED",
            )

        os.replace(staging, destination)
        fsync_directory(destination.parent)
        return destination
    except Exception:
        if staging.exists() and staging.is_dir() and not staging.is_symlink():
            shutil.rmtree(staging)
        elif staging.exists() or staging.is_symlink():
            staging.unlink()
        raise


def validate_backup_snapshot(snapshot_dir: Path) -> BackupValidation:
    snapshot_dir = Path(snapshot_dir)
    errors: list[str] = []
    if not snapshot_dir.is_dir() or snapshot_dir.is_symlink():
        return BackupValidation(False, ("snapshot root is missing, not a directory, or is a symlink",))

    manifest_path = snapshot_dir / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return BackupValidation(False, (f"manifest unreadable: {exc}",))

    if manifest.get("schema_version") != STATE_SCHEMA_VERSION:
        errors.append(f"unsupported schema_version {manifest.get('schema_version')!r}")
    records = manifest.get("sources")
    if not isinstance(records, list):
        errors.append("manifest sources is not a list")
        return BackupValidation(False, tuple(errors))

    for raw in records:
        if not isinstance(raw, dict):
            errors.append("invalid source record")
            continue
        label = raw.get("label")
        try:
            _validate_label(label)
        except BackupError as exc:
            errors.append(str(exc))
            continue
        existed = raw.get("existed")
        backup_rel = raw.get("backup_relative_path")
        expected = raw.get("backup_fingerprint")
        if existed:
            if not isinstance(backup_rel, str) or not isinstance(expected, str):
                errors.append(f"{label}: missing backup path/fingerprint")
                continue
            backup_path = snapshot_dir / backup_rel
            if not _lexically_within(backup_path, snapshot_dir):
                errors.append(f"{label}: backup path escapes snapshot root")
                continue
            if fingerprint_path(backup_path) != expected:
                errors.append(f"{label}: backup fingerprint mismatch")
        elif backup_rel is not None:
            errors.append(f"{label}: missing source unexpectedly has backup path")

    return BackupValidation(not errors, tuple(errors))


def _validate_label(label: object) -> None:
    if not isinstance(label, str) or not label or label in {".", ".."}:
        raise BackupError(f"Invalid backup label: {label!r}", code="RH_BACKUP_INVALID_LABEL")
    candidate = Path(label)
    if candidate.name != label or candidate.is_absolute() or "/" in label or "\\" in label:
        raise BackupError(f"Unsafe backup label: {label!r}", code="RH_BACKUP_INVALID_LABEL")


def _copy_path_symlink_safe(source: Path, destination: Path) -> None:
    st = source.lstat()
    if stat.S_ISLNK(st.st_mode):
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.symlink(os.readlink(source), destination)
        return
    if stat.S_ISREG(st.st_mode):
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        shutil.copy2(source, destination, follow_symlinks=False)
        _fsync_file(destination)
        return
    if stat.S_ISDIR(st.st_mode):
        destination.mkdir(mode=stat.S_IMODE(st.st_mode))
        with os.scandir(source) as iterator:
            entries = sorted(iterator, key=lambda item: os.fsencode(item.name))
        for entry in entries:
            _copy_path_symlink_safe(Path(entry.path), destination / entry.name)
        shutil.copystat(source, destination, follow_symlinks=False)
        fsync_directory(destination)
        return
    raise BackupError(
        f"Unsupported special filesystem entry in backup: {source}",
        code="RH_BACKUP_UNSUPPORTED_FILE_TYPE",
    )


def _filesystem_type(mode: int) -> str:
    if stat.S_ISLNK(mode):
        return "symlink"
    if stat.S_ISREG(mode):
        return "file"
    if stat.S_ISDIR(mode):
        return "directory"
    return "special"


def _durable_json_write(path: Path, payload: object) -> None:
    temporary = path.with_name(f".{path.name}.tmp-{uuid.uuid4().hex[:8]}")
    encoded = (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink(missing_ok=True)


def _fsync_file(path: Path) -> None:
    with path.open("rb") as handle:
        os.fsync(handle.fileno())


def _lexically_within(path: Path, root: Path) -> bool:
    path_abs = Path(os.path.abspath(path))
    root_abs = Path(os.path.abspath(root))
    try:
        return Path(os.path.commonpath([path_abs, root_abs])) == root_abs
    except ValueError:
        return False
