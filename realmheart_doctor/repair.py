"""Consent-gated repair planning: separate from diagnosis, explicit risk.

A plan is a proposal only.  Execution requires explicit per-action consent,
runs Doctor-owned bounded runners, and always verifies the outcome with a
fresh diagnosis instead of trusting an exit code.  Unobserved (UNKNOWN)
failures never produce a plan.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

from realmheart_maintenance.forensics import capability_state_satisfied
from realmheart_maintenance.manifest import ManifestRegistry
from realmheart_maintenance.packages import PACMAN_DEPENDENCY_PROVIDERS

from .classification import FailureClassification

RISK_SAFE = "SAFE"
RISK_CONFIRM = "CONFIRM"
RISK_PRIVILEGED = "PRIVILEGED_CONFIRM"

ACTION_INSTALL_PACKAGE = "INSTALL_MISSING_PACKAGE"
ACTION_REBUILD = "REBUILD_COMPONENT"
ACTION_RESTART_SERVICE = "RESTART_SERVICE"
ACTION_POST_CHECKS = "RUN_POST_REPAIR_CHECKS"
ACTION_REINSTALL = "REINSTALL_COMPONENT"

_ARTIFACT_FAILURES = frozenset({
    "COMPONENT_ARTIFACT_MISSING", "COMPONENT_ARTIFACT_INVALID", "COMPONENT_ARTIFACT_CORRUPT",
})


@dataclass(frozen=True)
class RepairAction:
    action_type: str
    risk: str
    description: str
    packages: tuple[str, ...] = ()
    targets: tuple[str, ...] = ()

    def fingerprint(self) -> str:
        digest = hashlib.sha256()
        for part in (self.action_type, *self.packages, *self.targets):
            digest.update(part.encode("utf-8", "surrogateescape"))
            digest.update(b"\x00")
        return digest.hexdigest()[:16]

    def to_dict(self) -> dict[str, object]:
        return {"action_type": self.action_type, "risk": self.risk, "description": self.description,
                "packages": list(self.packages), "targets": list(self.targets),
                "fingerprint": self.fingerprint()}


@dataclass(frozen=True)
class RepairPlan:
    component_id: str
    reason: str
    actions: tuple[RepairAction, ...]
    notes: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, object]:
        return {"component_id": self.component_id, "reason": self.reason,
                "actions": [item.to_dict() for item in self.actions],
                "notes": list(self.notes)}


@dataclass(frozen=True)
class RepairResult:
    status: str
    verified: bool
    detail: str | None = None


@dataclass(frozen=True)
class RepairEvidence:
    """Component-specific evidence gathered before planning."""

    packages: tuple[str, ...] = ()
    gated_packages: tuple[str, ...] = ()
    build_targets: tuple[str, ...] = ()
    service_units: tuple[str, ...] = ()
    missing_capability_ids: tuple[str, ...] = ()
    failed_capability_ids: tuple[str, ...] = ()
    notes: tuple[str, ...] = ()


@dataclass(frozen=True)
class RepairContext:
    component_id: str
    strategy_ids: tuple[str, ...]
    packages: tuple[str, ...] = ()
    gated_packages: tuple[str, ...] = ()
    build_targets: tuple[str, ...] = ()
    service_units: tuple[str, ...] = ()
    installer_bound: bool = False
    notes: tuple[str, ...] = ()


_PLANNERS = {
    "COMPONENT_ARTIFACT_MISSING": (
        RepairAction("REINSTALL_COMPONENT", "CONFIRM",
                     "reinstall the component so its required artifacts are restored"),
    ),
    "COMPONENT_ARTIFACT_INVALID": (
        RepairAction("REINSTALL_COMPONENT", "CONFIRM",
                     "reinstall the component so its required artifacts are restored to the declared type"),
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


def plan_repairs(
    component_id: str,
    classification: FailureClassification,
    *,
    context: RepairContext | None = None,
) -> RepairPlan | None:
    """Turn one evidence-backed classification into a consent-gated plan.

    Without a context the planner stays at the boundary vocabulary (reinstall
    advice only).  With a context it emits only actions a runner can actually
    perform for this component.
    """

    if classification.failure_class == "UNKNOWN" or not classification.evidence_ids:
        return None
    if context is None:
        actions = _PLANNERS.get(classification.failure_class)
        if not actions:
            return None
        return RepairPlan(component_id, classification.failure_class, actions)
    return _plan_from_context(classification, context)


def _plan_from_context(classification: FailureClassification, context: RepairContext) -> RepairPlan | None:
    failure = classification.failure_class
    actions: list[RepairAction] = []
    notes = list(context.notes)
    if failure in {"DEPENDENCY_MISSING", "DEPENDENCY_VERSION_MISMATCH"}:
        if "install_missing_package" in context.strategy_ids and context.packages:
            actions.append(RepairAction(
                ACTION_INSTALL_PACKAGE, RISK_PRIVILEGED,
                "install the missing packages with pacman (requires sudo)",
                packages=context.packages,
            ))
        if (failure == "DEPENDENCY_VERSION_MISMATCH" and context.build_targets
                and not context.installer_bound):
            if "rebuild_component" in context.strategy_ids:
                actions.append(RepairAction(
                    ACTION_REBUILD, RISK_CONFIRM,
                    "rebuild the component against the current dependency set",
                    targets=context.build_targets,
                ))
    elif failure in _ARTIFACT_FAILURES:
        if ("rebuild_component" in context.strategy_ids and context.build_targets
                and not context.installer_bound):
            actions.append(RepairAction(
                ACTION_REBUILD, RISK_CONFIRM,
                "rebuild and reinstall the component so its artifacts match the manifest",
                targets=context.build_targets,
            ))
        if not actions:
            notes.append("reinstall this component with the Realmheart installer; Doctor will not "
                         "overwrite installer-owned artifacts on its own")
    elif failure == "OBSERVED_FAILURE" and context.service_units and "restart_service" in context.strategy_ids:
        actions.append(RepairAction(
            ACTION_RESTART_SERVICE, RISK_CONFIRM,
            "restart the component's user services so they pick up the current state",
            targets=context.service_units,
        ))
    elif failure == "COMPONENT_DEPENDENCY_FAILURE":
        notes.append(
            "this component inherits its failure from a required dependency ("
            + ", ".join(classification.evidence_ids)
            + "); repair that component instead"
        )
    if not actions:
        return None
    actions.append(RepairAction(
        ACTION_POST_CHECKS, RISK_SAFE,
        "re-run the component's checks and verify the outcome",
    ))
    return RepairPlan(context.component_id, failure, tuple(actions), tuple(notes))



def plan_incident_repair(
    registry: ManifestRegistry, incident: dict[str, object],
) -> RepairPlan | None:
    """Build a repair plan from one persisted unresolved incident.

    Notification rendering must not re-run health probes merely to decide
    whether an action button is useful.  The incident already contains the
    exact evidence that produced its failure class, so reconstruct the same
    component context from the canonical manifest and plan from that snapshot.
    The actual ``repair-incident`` command re-diagnoses before executing, so a
    stale button can never force a repair after the component has recovered.
    """

    if incident.get("resolution_state") != "unresolved":
        return None
    component_id = incident.get("component_id")
    failure_class = incident.get("failure_class")
    confidence = incident.get("confidence")
    if not isinstance(component_id, str) or component_id not in registry.components:
        return None
    if not isinstance(failure_class, str) or not isinstance(confidence, str):
        return None

    evidence_ids: list[str] = []
    checks = incident.get("checks")
    if isinstance(checks, list):
        evidence_ids.extend(
            str(item.get("check_id"))
            for item in checks
            if isinstance(item, dict) and isinstance(item.get("check_id"), str)
        )
    capabilities = incident.get("capabilities")
    capability_ids: list[str] = []
    if isinstance(capabilities, list):
        capability_ids = [
            str(item.get("capability_id"))
            for item in capabilities
            if isinstance(item, dict)
            and isinstance(item.get("capability_id"), str)
            and item.get("state") in {"missing", "failed"}
        ]
    if failure_class in {"DEPENDENCY_MISSING", "DEPENDENCY_VERSION_MISMATCH"}:
        evidence_ids = capability_ids
    if not evidence_ids:
        return None

    component = registry.components[component_id]
    packages: list[str] = []
    gated: list[str] = []
    for capability_id in capability_ids:
        capability = registry.capabilities.get(capability_id)
        if capability is None:
            continue
        provider = PACMAN_DEPENDENCY_PROVIDERS.get(capability.dependency_id)
        if provider is None:
            continue
        (packages if provider.automatic else gated).extend(provider.packages)

    targets = tuple(
        unit.cmake_target
        for unit in registry.build_units.values()
        if component_id in unit.component_ids and unit.cmake_target
    )
    units = tuple(
        Path(artifact.path).name
        for artifact in registry.artifacts.values()
        if artifact.component_id == component_id and artifact.type == "service"
    )
    notes: list[str] = []
    if component.requires_installer_binding:
        notes.append(
            "this component owns privileged installer-bound artifacts; Doctor will not "
            "perform privileged installs on its own"
        )
    if gated:
        notes.append(
            "these packages are never installed automatically: "
            + ", ".join(dict.fromkeys(gated))
        )

    classification = FailureClassification(
        failure_class, confidence, tuple(dict.fromkeys(evidence_ids))
    )
    return plan_repairs(
        component_id, classification,
        context=RepairContext(
            component_id=component_id,
            strategy_ids=component.repair_strategy_ids,
            packages=tuple(dict.fromkeys(packages)),
            gated_packages=tuple(dict.fromkeys(gated)),
            build_targets=targets,
            service_units=units,
            installer_bound=component.requires_installer_binding,
            notes=tuple(notes),
        ),
    )

def assess_repair_evidence(
    registry: ManifestRegistry,
    component_id: str,
    *,
    prober=None,
) -> RepairEvidence:
    """Observe what this component is missing before proposing any repair."""

    if prober is None:
        from .acceptance import _probe_capability

        prober = _probe_capability
    component = registry.components[component_id]
    packages: list[str] = []
    gated: list[str] = []
    missing: list[str] = []
    failed: list[str] = []
    for capability in registry.capabilities.values():
        if capability.component_id not in (None, component_id):
            continue
        observation = prober(capability, registry=registry)
        if capability_state_satisfied(registry, capability, observation.state):
            continue
        if observation.state == "missing":
            missing.append(capability.id)
        elif observation.state == "failed":
            failed.append(capability.id)
        else:
            continue
        provider = PACMAN_DEPENDENCY_PROVIDERS.get(capability.dependency_id)
        if provider is None:
            continue
        (packages if provider.automatic else gated).extend(provider.packages)
    targets = tuple(
        unit.cmake_target
        for unit in registry.build_units.values()
        if component_id in unit.component_ids and unit.cmake_target
    )
    units = tuple(
        Path(artifact.path).name
        for artifact in registry.artifacts.values()
        if artifact.component_id == component_id and artifact.type == "service"
    )
    notes: list[str] = []
    if component.requires_installer_binding:
        notes.append("this component owns privileged installer-bound artifacts; Doctor will not "
                     "perform privileged installs on its own")
    if gated:
        notes.append("these packages are never installed automatically: " + ", ".join(dict.fromkeys(gated)))
    return RepairEvidence(
        packages=tuple(dict.fromkeys(packages)),
        gated_packages=tuple(dict.fromkeys(gated)),
        build_targets=targets,
        service_units=units,
        missing_capability_ids=tuple(sorted(missing)),
        failed_capability_ids=tuple(sorted(failed)),
        notes=tuple(notes),
    )
