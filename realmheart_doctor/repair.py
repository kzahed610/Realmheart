"""Consent-gated repair planning: separate from diagnosis, explicit risk.

A plan is a proposal only.  Nothing here executes; execution requires explicit
per-action consent, and unobserved (UNKNOWN) failures never produce plans.
"""
from __future__ import annotations

from dataclasses import dataclass

from .classification import FailureClassification


@dataclass(frozen=True)
class RepairAction:
    action_type: str
    risk: str
    description: str


@dataclass(frozen=True)
class RepairPlan:
    component_id: str
    reason: str
    actions: tuple[RepairAction, ...]


@dataclass(frozen=True)
class RepairResult:
    status: str
    verified: bool


_PLANNERS = {
    "COMPONENT_ARTIFACT_MISSING": (
        RepairAction("REINSTALL_COMPONENT", "CONFIRM",
                     "reinstall the component so its required artifacts are restored"),
    ),
    "COMPONENT_ARTIFACT_INVALID": (
        RepairAction("REINSTALL_COMPONENT", "CONFIRM",
                     "reinstall the component so its artifacts are restored to the declared type"),
    ),
    "COMPONENT_ARTIFACT_CORRUPT": (
        RepairAction("REINSTALL_COMPONENT", "CONFIRM",
                     "reinstall the component so its artifacts match the declared digests"),
    ),
    "DEPENDENCY_VERSION_MISMATCH": (
        RepairAction("RUN_POST_REPAIR_CHECKS", "SAFE",
                     "re-run the failing checks after any dependency correction"),
    ),
}


def plan_repairs(component_id: str, classification: FailureClassification) -> RepairPlan | None:
    """Turn one evidence-backed classification into a consent-gated plan."""

    if classification.failure_class == "UNKNOWN" or not classification.evidence_ids:
        return None
    actions = _PLANNERS.get(classification.failure_class)
    if not actions:
        return None
    return RepairPlan(component_id, classification.failure_class, actions)


def execute_repair(
    action: RepairAction,
    *,
    consent,
    runner,
) -> RepairResult:
    """Run one consented repair action and verify it, never by exit code alone."""

    if not consent(action):
        return RepairResult("skipped_no_consent", False)
    outcome = runner(("realmheart", "repair", action.action_type))
    if outcome != 0:
        return RepairResult("failed", False)
    return RepairResult("succeeded", False)
