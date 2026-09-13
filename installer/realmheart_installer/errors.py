"""Structured installer errors.

Errors expose stable machine-readable codes without leaking arbitrary exception
text into future reports by default.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(eq=False)
class InstallerError(Exception):
    message: str
    code: str = "RH_INSTALLER_ERROR"
    stage: str | None = None
    details: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        super().__init__(self.message)




class TerminationSignalInterrupt(KeyboardInterrupt):
    """Controlled process interruption raised from a termination signal handler.

    Subclassing ``KeyboardInterrupt`` lets existing interruption boundaries treat
    SIGTERM like Ctrl+C while retaining the original signal identity for durable
    recovery metadata.
    """

    def __init__(self, signum: int) -> None:
        self.signum = int(signum)
        super().__init__(f"signal:{self.signum}")


def interruption_reason(exc: BaseException) -> str:
    """Return a stable interruption vocabulary for transaction metadata."""

    if isinstance(exc, TerminationSignalInterrupt):
        try:
            import signal
            return signal.Signals(exc.signum).name.lower()
        except (ValueError, AttributeError):
            return f"signal_{exc.signum}"
    if isinstance(exc, KeyboardInterrupt):
        return "keyboard_interrupt"
    return type(exc).__name__.lower()


class RootExecutionError(InstallerError):
    def __init__(self) -> None:
        super().__init__(
            "Do not run Realmheart Installer as root. "
            "The installer requests elevation only for operations that need it.",
            code="RH_ROOT_EXECUTION_REFUSED",
            stage="startup",
        )


class LockHeldError(InstallerError):
    def __init__(self, lock_path: str) -> None:
        super().__init__(
            "Another Realmheart installation is currently in progress.",
            code="RH_INSTALLER_LOCK_HELD",
            stage="startup",
            details={"lock_path": lock_path},
        )


class JournalCorruptError(InstallerError):
    def __init__(self, message: str, *, line_number: int | None = None) -> None:
        details: dict[str, Any] = {}
        if line_number is not None:
            details["line_number"] = line_number
        super().__init__(
            message,
            code="RH_JOURNAL_CORRUPT",
            stage="recovery",
            details=details,
        )


class PreconditionFailedError(InstallerError):
    def __init__(self, target: str, reason: str) -> None:
        super().__init__(
            f"Mutation precondition failed for {target}: {reason}",
            code="RH_PRECONDITION_DRIFT",
            stage="mutation",
            details={"target": target, "reason": reason},
        )


class UnsafePathError(InstallerError):
    def __init__(self, target: str, allowed_root: str, reason: str) -> None:
        super().__init__(
            f"Refusing unsafe path operation on {target}: {reason}",
            code="RH_UNSAFE_PATH",
            stage="mutation",
            details={"target": target, "allowed_root": allowed_root, "reason": reason},
        )


class OperationExecutionError(InstallerError):
    def __init__(self, operation_id: str, reason: str) -> None:
        super().__init__(
            f"Operation {operation_id} failed: {reason}",
            code="RH_OPERATION_FAILED",
            stage="mutation",
            details={"operation_id": operation_id, "reason": reason},
        )


class BackupError(InstallerError):
    def __init__(self, message: str, *, code: str = "RH_BACKUP_FAILED", details: dict[str, Any] | None = None) -> None:
        super().__init__(message, code=code, stage="backup", details=details or {})


class BaselineInvalidError(BackupError):
    def __init__(self, message: str) -> None:
        super().__init__(message, code="RH_BASELINE_INVALID")


class StagingError(InstallerError):
    def __init__(self, message: str, *, code: str = "RH_STAGING_FAILED", details: dict[str, Any] | None = None) -> None:
        super().__init__(message, code=code, stage="staging", details=details or {})


class ManagedBlockConflictError(InstallerError):
    def __init__(self, target: str, reason: str) -> None:
        super().__init__(
            f"Managed block conflict in {target}: {reason}",
            code="RH_MANAGED_BLOCK_CONFLICT",
            stage="planning",
            details={"target": target, "reason": reason},
        )


class PlanningInspectionError(InstallerError):
    def __init__(self, target: str, reason: str) -> None:
        super().__init__(
            f"Cannot safely inspect planned target {target}: {reason}",
            code="RH_PLAN_INSPECTION_FAILED",
            stage="planning",
            details={"target": target, "reason": reason},
        )


class BuildStageError(InstallerError):
    def __init__(self, message: str, *, code: str = "RH_BUILD_STAGE_FAILED", details: dict[str, Any] | None = None) -> None:
        super().__init__(message, code=code, stage="build_stage", details=details or {})


class ConfigurationIntegrationError(InstallerError):
    def __init__(self, message: str, *, code: str = "RH_CONFIG_INTEGRATION_FAILED", details: dict[str, Any] | None = None) -> None:
        super().__init__(message, code=code, stage="configuration", details=details or {})
