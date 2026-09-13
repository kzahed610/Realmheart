"""Phase-16 final decision and rollback result models."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class FinalAction(str, Enum):
    KEEP = "keep"
    RESTORE_PREVIOUS = "restore_previous"
    RESTORE_BASELINE = "restore_baseline"


class FinalSeverity(str, Enum):
    SUCCESS = "success"
    SUCCESS_WITH_WARNINGS = "success_with_warnings"
    DEGRADED = "degraded"
    CRITICAL = "critical"


@dataclass(frozen=True)
class FinalDecisionOption:
    action: FinalAction
    label: str
    recommended: bool
    destructive: bool
    reason: str


@dataclass(frozen=True)
class FinalDecisionPlan:
    severity: FinalSeverity
    install_health: str
    activation_state: str
    runtime_health: str
    headline: str
    summary: str
    options: tuple[FinalDecisionOption, ...]
    requires_explicit_choice: bool
    default_action: FinalAction | None
    core_critical: bool
    fx_critical: bool
    doctor_recommendation: str | None = None
    doctor_summary: str | None = None


@dataclass(frozen=True)
class RollbackStatus:
    attempted: bool
    ok: bool
    restored_target: str | None
    errors: tuple[str, ...]
    previous_receipt_preserved: bool


@dataclass(frozen=True)
class FinalizationResult:
    action: FinalAction
    disposition: str
    exit_code: int
    receipt_path: str | None
    rollback: RollbackStatus
    warnings: tuple[str, ...]
    summary: str
