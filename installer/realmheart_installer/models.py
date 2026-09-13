"""Core installer data model.

The enums mirror the implementation plan so logs, reports, recovery and future
Doctor-facing state share stable vocabulary.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field, is_dataclass
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Any


class StringEnum(str, Enum):
    """String-valued Enum with JSON-friendly semantics."""


class ComponentCategory(StringEnum):
    CORE = "core"
    ESSENTIAL = "essential"
    QOL = "qol"
    OPTIONAL = "optional"
    EXPERIMENTAL = "experimental"
    FX = "fx"


class ComponentState(StringEnum):
    PENDING = "pending"
    RUNNING = "running"
    PASS = "pass"
    FAILED = "failed"
    BLOCKED = "blocked"
    SKIPPED = "skipped"
    WARNING = "warning"


class TransactionState(StringEnum):
    CREATED = "created"
    PREFLIGHT = "preflight"
    PLANNED = "planned"
    BACKUP = "backup"
    APPLYING = "applying"
    VERIFYING = "verifying"
    DECISION_REQUIRED = "decision_required"
    COMMITTING = "committing"
    COMMITTED = "committed"
    INTERRUPTED = "interrupted"
    FAILED = "failed"
    ROLLING_BACK = "rolling_back"
    ROLLED_BACK = "rolled_back"
    ROLLBACK_FAILED = "rollback_failed"


class OperationState(StringEnum):
    INTENT = "intent"
    STARTED = "started"
    COMPLETED = "completed"
    FAILED = "failed"
    ROLLBACK_STARTED = "rollback_started"
    ROLLED_BACK = "rolled_back"
    ROLLBACK_FAILED = "rollback_failed"


class InstallMode(StringEnum):
    FRESH = "fresh"
    REINSTALL = "reinstall"
    UPGRADE = "upgrade"
    DOWNGRADE = "downgrade"


class FxCompatibility(StringEnum):
    COMPATIBLE = "compatible"
    UNKNOWN = "unknown"
    INCOMPATIBLE = "incompatible"


class HealthState(StringEnum):
    SUCCESS = "success"
    SUCCESS_WITH_WARNINGS = "success_with_warnings"
    CORE_FAILURE = "core_failure"
    FX_FAILURE = "fx_failure"
    CORE_SUCCESS_FX_FAILURE = "core_success_fx_failure"
    INTERRUPTED = "interrupted"
    ROLLED_BACK = "rolled_back"
    ROLLBACK_FAILED = "rollback_failed"


class Reversibility(StringEnum):
    EXACT = "exact"
    GUARDED = "guarded"
    BEST_EFFORT = "best_effort"
    NONE = "none"


class OperationKind(StringEnum):
    CREATE_DIRECTORY = "create_directory"
    WRITE_FILE = "write_file"
    MOVE_PATH = "move_path"
    REMOVE_PATH = "remove_path"


@dataclass(frozen=True)
class MutationPrecondition:
    kind: str
    expected_fingerprint: str | None = None
    expected_exists: bool | None = None


@dataclass(frozen=True)
class OperationSafety:
    reversibility: Reversibility
    precondition: MutationPrecondition | None = None
    rollback_source: str | None = None


@dataclass
class ComponentResult:
    component_id: str
    state: ComponentState
    stage: str
    started_at: datetime | None = None
    finished_at: datetime | None = None
    reason: str | None = None
    error_code: str | None = None
    blocked_by: tuple[str, ...] = ()
    warnings: tuple[str, ...] = ()
    operation_ids: tuple[str, ...] = ()


@dataclass
class TransactionRecord:
    schema_version: int
    transaction_id: str
    installer_version: str
    state: TransactionState
    created_at: datetime
    updated_at: datetime
    source_root: Path | None = None
    dry_run: bool = False
    install_mode: InstallMode | None = None
    installation_origin: str | None = None
    current_version: str | None = None
    target_version: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def create(
        cls,
        *,
        schema_version: int,
        transaction_id: str,
        installer_version: str,
        source_root: Path | None,
        dry_run: bool,
    ) -> "TransactionRecord":
        now = datetime.now(timezone.utc)
        return cls(
            schema_version=schema_version,
            transaction_id=transaction_id,
            installer_version=installer_version,
            state=TransactionState.CREATED,
            created_at=now,
            updated_at=now,
            source_root=source_root,
            dry_run=dry_run,
        )

    def transition(self, state: TransactionState) -> None:
        self.state = state
        self.updated_at = datetime.now(timezone.utc)


def to_jsonable(value: Any) -> Any:
    """Convert installer models into deterministic JSON-compatible values."""

    if isinstance(value, Enum):
        return value.value
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, datetime):
        return value.astimezone(timezone.utc).isoformat()
    if is_dataclass(value):
        return {key: to_jsonable(val) for key, val in asdict(value).items()}
    if isinstance(value, dict):
        return {str(key): to_jsonable(val) for key, val in value.items()}
    if isinstance(value, (tuple, list, set)):
        return [to_jsonable(item) for item in value]
    return value
