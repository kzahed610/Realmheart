"""Target-user identity, XDG resolution and transaction context."""

from __future__ import annotations

import json
import os
import secrets
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Mapping

from .constants import (
    INSTALLER_STATE_DIRNAME,
    INSTALLER_VERSION,
    LOCK_FILENAME,
    REALMHEART_STATE_DIRNAME,
    RECOVERY_RESERVE_BYTES,
    RECOVERY_RESERVE_FILENAME,
    STATE_SCHEMA_VERSION,
    TRANSACTION_PREFIX,
)
from .durability import fsync_directory
from .errors import InstallerError, RootExecutionError
from .models import TransactionRecord, to_jsonable


def ensure_not_root(*, euid: int | None = None) -> None:
    effective_uid = os.geteuid() if euid is None else euid
    if effective_uid == 0:
        raise RootExecutionError()


def generate_transaction_id(*, now: datetime | None = None, random_hex: str | None = None) -> str:
    timestamp = (now or datetime.now()).strftime("%Y%m%d-%H%M%S")
    suffix = (random_hex or secrets.token_hex(2)).upper()
    return f"{TRANSACTION_PREFIX}-{timestamp}-{suffix}"


def _absolute_xdg(value: str | None, fallback: Path) -> Path:
    if value:
        candidate = Path(value).expanduser()
        if candidate.is_absolute():
            return candidate
    return fallback


@dataclass(frozen=True)
class XdgPaths:
    home: Path
    config_home: Path
    state_home: Path
    data_home: Path
    cache_home: Path
    runtime_dir: Path

    installer_state: Path
    installer_data: Path
    installer_cache: Path
    realmheart_state: Path
    transactions: Path
    logs: Path
    reports: Path
    backups: Path
    baseline_backup: Path
    version_backups: Path
    lock_path: Path

    @classmethod
    def resolve(
        cls,
        *,
        env: Mapping[str, str] | None = None,
        uid: int | None = None,
    ) -> "XdgPaths":
        environ = os.environ if env is None else env
        home_value = environ.get("HOME")
        if not home_value:
            raise ValueError("HOME is required to resolve installer paths")
        home = Path(home_value).expanduser()
        if not home.is_absolute():
            raise ValueError("HOME must be an absolute path")

        config_home = _absolute_xdg(environ.get("XDG_CONFIG_HOME"), home / ".config")
        state_home = _absolute_xdg(environ.get("XDG_STATE_HOME"), home / ".local/state")
        data_home = _absolute_xdg(environ.get("XDG_DATA_HOME"), home / ".local/share")
        cache_home = _absolute_xdg(environ.get("XDG_CACHE_HOME"), home / ".cache")

        resolved_uid = os.getuid() if uid is None else uid
        installer_state = state_home / INSTALLER_STATE_DIRNAME
        # XDG_RUNTIME_DIR is intentionally not guessed as /run/user/<uid> when
        # absent. That path may not exist on non-systemd/minimal environments,
        # and creating it is not the installer's job. Fall back to private
        # installer state as required by the implementation plan.
        runtime_fallback = installer_state / "runtime"
        runtime_dir = _absolute_xdg(environ.get("XDG_RUNTIME_DIR"), runtime_fallback)

        installer_data = data_home / INSTALLER_STATE_DIRNAME
        installer_cache = cache_home / INSTALLER_STATE_DIRNAME
        realmheart_state = state_home / REALMHEART_STATE_DIRNAME

        return cls(
            home=home,
            config_home=config_home,
            state_home=state_home,
            data_home=data_home,
            cache_home=cache_home,
            runtime_dir=runtime_dir,
            installer_state=installer_state,
            installer_data=installer_data,
            installer_cache=installer_cache,
            realmheart_state=realmheart_state,
            transactions=installer_state / "transactions",
            logs=installer_state / "logs",
            reports=installer_state / "reports",
            backups=installer_data / "backups",
            baseline_backup=installer_data / "backups/baseline",
            version_backups=installer_data / "backups/versions",
            lock_path=runtime_dir / LOCK_FILENAME,
        )

    def prepare_lock_parent(self) -> None:
        """Create the private fallback runtime root without weakening XDG runtime dirs."""

        fallback_runtime = self.installer_state / "runtime"
        if self.runtime_dir == fallback_runtime:
            _ensure_private_directory(self.installer_state)
            _ensure_private_directory(self.runtime_dir)

    def create_private_roots(self) -> None:
        # Create Realmheart-owned namespace roots explicitly before children so
        # recursive mkdir never leaves an intermediate `realmheart-installer/`
        # at the process umask's more permissive default mode.
        for path in (
            self.installer_state,
            self.installer_data,
            self.installer_cache,
            self.realmheart_state,
            self.transactions,
            self.logs,
            self.reports,
            self.backups,
            self.version_backups,
            self.installer_cache / "temporary",
        ):
            _ensure_private_directory(path)


def _ensure_private_directory(path: Path) -> None:
    path = Path(path)
    if path.is_symlink():
        raise InstallerError(
            f"Refusing symlinked Realmheart private state directory: {path}",
            code="RH_PRIVATE_STATE_UNSAFE", stage="startup",
        )
    existed = path.exists()
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink() or not path.is_dir():
        raise InstallerError(
            f"Realmheart private state path is not a real directory: {path}",
            code="RH_PRIVATE_STATE_UNSAFE", stage="startup",
        )
    os.chmod(path, 0o700)
    # fsync both the directory metadata and, when newly linked, its parent's
    # directory entry. EIO/ENOSPC propagate through the strict helper.
    fsync_directory(path)
    if not existed:
        fsync_directory(path.parent)


@dataclass
class InstallContext:
    paths: XdgPaths
    transaction: TransactionRecord
    transaction_dir: Path

    @classmethod
    def create(
        cls,
        *,
        paths: XdgPaths,
        source_root: Path | None = None,
        dry_run: bool = False,
        transaction_id: str | None = None,
    ) -> "InstallContext":
        paths.create_private_roots()
        txid = transaction_id or generate_transaction_id()
        transaction_dir = paths.transactions / txid
        transaction_dir.mkdir(mode=0o700)
        # The transaction directory name itself is recovery-critical metadata.
        # Make the parent-directory entry durable before relying on files inside
        # it for crash reconstruction.
        fsync_directory(transaction_dir.parent)

        transaction = TransactionRecord.create(
            schema_version=STATE_SCHEMA_VERSION,
            transaction_id=txid,
            installer_version=INSTALLER_VERSION,
            source_root=source_root,
            dry_run=dry_run,
        )
        context = cls(paths=paths, transaction=transaction, transaction_dir=transaction_dir)
        context.persist_summary()
        return context

    @classmethod
    def load_existing(cls, *, paths: XdgPaths, transaction_id: str) -> "InstallContext":
        """Load one durable transaction without creating or mutating it.

        Crash recovery must reconstruct state from the transaction directory that
        existed before this process started.  The directory name is the authority
        for which transaction is being inspected; JSON fields are validated
        against it rather than being allowed to redirect recovery elsewhere.
        """

        if Path(transaction_id).name != transaction_id or not transaction_id.startswith(f"{TRANSACTION_PREFIX}-"):
            raise ValueError("invalid Realmheart transaction ID")
        transaction_dir = paths.transactions / transaction_id
        if transaction_dir.is_symlink() or not transaction_dir.is_dir():
            raise ValueError("transaction directory is missing or is a symlink")
        summary_path = transaction_dir / "transaction.json"
        if summary_path.is_symlink() or not summary_path.is_file():
            raise ValueError("transaction summary is missing or is a symlink")
        payload = json.loads(summary_path.read_text(encoding="utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("transaction summary must be a JSON object")
        if str(payload.get("transaction_id")) != transaction_id:
            raise ValueError("transaction summary identity does not match its directory")
        schema_version = int(payload.get("schema_version"))
        if schema_version != STATE_SCHEMA_VERSION:
            raise ValueError(f"unsupported transaction schema {schema_version}")

        created_at = datetime.fromisoformat(str(payload["created_at"]))
        updated_at = datetime.fromisoformat(str(payload["updated_at"]))
        source_raw = payload.get("source_root")
        mode_raw = payload.get("install_mode")
        from .models import InstallMode, TransactionState
        transaction = TransactionRecord(
            schema_version=schema_version,
            transaction_id=transaction_id,
            installer_version=str(payload["installer_version"]),
            state=TransactionState(str(payload["state"])),
            created_at=created_at,
            updated_at=updated_at,
            source_root=Path(str(source_raw)) if source_raw else None,
            dry_run=bool(payload.get("dry_run", False)),
            install_mode=InstallMode(str(mode_raw)) if mode_raw else None,
            installation_origin=str(payload["installation_origin"]) if payload.get("installation_origin") is not None else None,
            current_version=str(payload["current_version"]) if payload.get("current_version") is not None else None,
            target_version=str(payload["target_version"]) if payload.get("target_version") is not None else None,
            metadata=dict(payload.get("metadata") or {}),
        )
        return cls(paths=paths, transaction=transaction, transaction_dir=transaction_dir)

    @property
    def journal_path(self) -> Path:
        return self.transaction_dir / "journal.jsonl"

    @property
    def recovery_reserve_path(self) -> Path:
        return self.transaction_dir / RECOVERY_RESERVE_FILENAME

    @property
    def preimage_dir(self) -> Path:
        path = self.transaction_dir / "preimages"
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
        return path


    def ensure_recovery_reserve(self) -> None:
        """Reserve a tiny amount of real disk space for emergency recovery metadata.

        The file is deliberately allocated immediately before a mutation class
        that may need emergency reporting.  If a later write fails with ENOSPC,
        recovery reporting can remove this file
        and retry, avoiding the absurd failure mode where disk exhaustion also
        prevents the installer from explaining that disk exhaustion happened.
        """

        target = self.recovery_reserve_path
        if target.is_file():
            return
        if target.exists() or target.is_symlink():
            raise OSError(f"recovery reserve path is not a regular file: {target}")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        fd = os.open(target, flags, 0o600)
        try:
            remaining = RECOVERY_RESERVE_BYTES
            block = b"\0" * min(4096, RECOVERY_RESERVE_BYTES)
            while remaining > 0:
                chunk = block if remaining >= len(block) else block[:remaining]
                written = os.write(fd, chunk)
                if written <= 0:
                    raise OSError("short write while allocating recovery reserve")
                remaining -= written
            os.fsync(fd)
        finally:
            os.close(fd)
        fsync_directory(target.parent)

    def release_recovery_reserve(self) -> None:
        target = self.recovery_reserve_path
        if target.exists() or target.is_symlink():
            target.unlink(missing_ok=True)
            fsync_directory(target.parent)

    def persist_recovery_json(self, filename: str, payload: object) -> Path:
        """Persist recovery metadata, sacrificing the emergency reserve on ENOSPC."""

        try:
            return self.persist_json(filename, payload)
        except OSError as exc:
            import errno
            if exc.errno != errno.ENOSPC:
                raise
            self.release_recovery_reserve()
            return self.persist_json(filename, payload)

    def persist_summary(self) -> None:
        self.persist_json("transaction.json", self.transaction)

    def persist_json(self, filename: str, payload: object) -> Path:
        """Durably persist one transaction-side JSON artifact.

        Dry-run deliberately does not construct InstallContext, so this helper is
        only reachable from a real transaction and cannot accidentally make a
        read-only plan persistent.
        """
        if Path(filename).name != filename or not filename.endswith(".json"):
            raise ValueError("transaction JSON filename must be one safe basename ending in .json")
        target = self.transaction_dir / filename
        temporary = target.with_name(f".{target.name}.tmp")
        encoded = json.dumps(to_jsonable(payload), indent=2, sort_keys=True) + "\n"
        with temporary.open("w", encoding="utf-8") as handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, target)
        fsync_directory(target.parent)
        return target
