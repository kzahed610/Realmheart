"""Terminal rendering for Phase-16 decision/final result."""
from __future__ import annotations
from .models import FinalDecisionPlan, FinalizationResult


def render_final_decision(plan: FinalDecisionPlan) -> str:
    lines = ["Realmheart final decision", "", plan.headline, plan.summary, "", f"Install health .... {plan.install_health}", f"Activation ........ {plan.activation_state}", f"Runtime ........... {plan.runtime_health}"]
    if plan.doctor_recommendation:
        lines.append(f"Doctor ............ {plan.doctor_recommendation}")
    lines += ["", "Options"]
    for index, item in enumerate(plan.options, start=1):
        marker = " [recommended]" if item.recommended else ""
        lines.append(f"  [{index}] {item.label}{marker}")
    return "\n".join(lines)


def render_finalization_result(result: FinalizationResult) -> str:
    lines = ["Realmheart finalization", "", f"Action ............ {result.action.value}", f"Disposition ....... {result.disposition}", f"Result ............ {result.summary}"]
    if result.receipt_path:
        lines.append(f"Installed receipt . {result.receipt_path}")
    if result.rollback.attempted:
        lines.append(f"Rollback .......... {'PASS' if result.rollback.ok else 'FAILED'}")
        for error in result.rollback.errors:
            lines.append(f"  - {error}")
    if result.warnings:
        lines.append("Warnings")
        for warning in result.warnings:
            lines.append(f"  - {warning}")
    return "\n".join(lines)
