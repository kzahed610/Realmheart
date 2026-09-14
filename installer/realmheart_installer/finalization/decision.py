"""Classify observed verification state into explicit final user paths."""
from __future__ import annotations

from realmheart_doctor.models import AcceptanceAssessment, AcceptanceRecommendation

from ..models import InstallMode
from ..planning.models import BackupKind, InstallationPlan
from ..verification.models import ComponentHealthState, InstallHealthState, VerificationReport
from .models import FinalAction, FinalDecisionOption, FinalDecisionPlan, FinalSeverity


def build_final_decision(
    plan: InstallationPlan,
    report: VerificationReport,
    doctor: AcceptanceAssessment | None = None,
) -> FinalDecisionPlan:
    component = {item.component_id: item for item in report.components}
    core = component.get("realmheart-core")
    fx = component.get("realmheart-fx")
    core_critical = bool(core and core.state in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED})
    fx_critical = bool(plan.fx_plan.required and fx and fx.state is ComponentHealthState.FAILED)
    doctor_rec = doctor.recommendation if doctor is not None else None
    doctor_summary = doctor.summary if doctor is not None else None
    rollback_options = _rollback_options(plan, recommended=True)

    if report.install_health is InstallHealthState.HEALTHY:
        if doctor_rec is AcceptanceRecommendation.REVERT_RECOMMENDED:
            options = rollback_options + (
                FinalDecisionOption(
                    FinalAction.KEEP,
                    "Keep installation despite Doctor recommendation",
                    False,
                    False,
                    "installer verification passed, but Doctor found independent critical evidence",
                ),
            )
            return FinalDecisionPlan(
                FinalSeverity.CRITICAL,
                report.install_health.value,
                report.activation.state.value,
                report.activation.runtime_health.value,
                "Installer verification passed, but Realmheart Doctor recommends reverting.",
                doctor_summary,
                options,
                True,
                None,
                True,
                fx_critical,
                doctor_rec.value if doctor_rec is not None else None,
                doctor_summary,
            )

        if doctor_rec is AcceptanceRecommendation.INDETERMINATE:
            options = rollback_options + (
                FinalDecisionOption(
                    FinalAction.KEEP,
                    "Keep verified installation despite Doctor uncertainty",
                    False,
                    False,
                    "installer verification passed, but Doctor could not establish an independent acceptance verdict",
                ),
            )
            doctor_indeterminate_summary = doctor_summary or "Doctor could not establish an independent acceptance verdict."
            if report.activation.state.value == "pending_session_restart":
                summary = (
                    "Install-time verification passed. A fresh Hyprland session is still required before runtime can become Last Known Good. "
                    + doctor_indeterminate_summary
                )
            else:
                summary = doctor_indeterminate_summary
            return FinalDecisionPlan(
                FinalSeverity.SUCCESS_WITH_WARNINGS,
                report.install_health.value,
                report.activation.state.value,
                report.activation.runtime_health.value,
                "Realmheart installation verified, but Doctor could not establish an independent verdict.",
                summary,
                options,
                True,
                None,
                core_critical,
                fx_critical,
                doctor_rec.value,
                doctor_summary,
            )

        warning_state = bool(report.warnings) or doctor_rec in {
            AcceptanceRecommendation.KEEP_WITH_WARNINGS,
            AcceptanceRecommendation.INDETERMINATE,
        }
        severity = FinalSeverity.SUCCESS_WITH_WARNINGS if warning_state else FinalSeverity.SUCCESS
        headline = "Realmheart installation verified with warnings." if warning_state else "Realmheart installation verified successfully."
        if report.activation.state.value == "pending_session_restart":
            summary = "Install-time verification passed. A fresh Hyprland session is still required before runtime can become Last Known Good."
        else:
            summary = "Install-time verification passed and the accepted activation state is recorded from observed machine state."
        if doctor_rec is AcceptanceRecommendation.INDETERMINATE:
            summary += " Doctor could not establish an independent acceptance verdict; installer verification remains authoritative."
        elif doctor_rec is AcceptanceRecommendation.KEEP_WITH_WARNINGS:
            summary += " Doctor found noncritical warnings; they remain recorded with the final decision."
        return FinalDecisionPlan(
            severity,
            report.install_health.value,
            report.activation.state.value,
            report.activation.runtime_health.value,
            headline,
            summary,
            (FinalDecisionOption(FinalAction.KEEP, "Keep verified installation", True, False, "all required install-time verification passed"),),
            False,
            FinalAction.KEEP,
            core_critical,
            fx_critical,
            doctor_rec.value if doctor_rec is not None else None,
            doctor_summary,
        )

    if report.install_health is InstallHealthState.DEGRADED:
        doctor_revert = doctor_rec is AcceptanceRecommendation.REVERT_RECOMMENDED
        options = (
            FinalDecisionOption(
                FinalAction.KEEP,
                "Keep degraded installation",
                not doctor_revert,
                False,
                "Core remains viable but noncritical failures will be recorded honestly",
            ),
        ) + _rollback_options(plan, recommended=doctor_revert)
        return FinalDecisionPlan(
            FinalSeverity.CRITICAL if doctor_revert else FinalSeverity.DEGRADED,
            report.install_health.value,
            report.activation.state.value,
            report.activation.runtime_health.value,
            "Realmheart Doctor recommends reverting the degraded installation." if doctor_revert else "Realmheart installation completed with noncritical degradation.",
            doctor_summary if doctor_revert else "Realmheart Core is still viable. Keeping the state requires an explicit decision and the receipt will record the degraded components.",
            options,
            True,
            None,
            core_critical or doctor_revert,
            fx_critical,
            doctor_rec.value if doctor_rec is not None else None,
            doctor_summary,
        )

    # Installer failure is always authoritative. Doctor cannot green-light it.
    options = rollback_options + (
        FinalDecisionOption(FinalAction.KEEP, "Keep failed installation anyway", False, False, "records the failed state honestly; not runtime Last Known Good"),
    )
    if not rollback_options:
        options = (FinalDecisionOption(FinalAction.KEEP, "Keep failed installation anyway", True, False, "no validated rollback target is available"),)
    recommendation = "Restore the previous Realmheart state." if plan.mode is not InstallMode.FRESH else "Restore the pre-Realmheart state."
    if fx_critical:
        recommendation = "Required Realmheart FX failed. " + recommendation
    return FinalDecisionPlan(
        FinalSeverity.CRITICAL,
        report.install_health.value,
        report.activation.state.value,
        report.activation.runtime_health.value,
        "Realmheart failed critical post-install verification.",
        recommendation,
        options,
        True,
        None,
        core_critical or fx_critical,
        fx_critical,
        doctor_rec.value if doctor_rec is not None else None,
        doctor_summary,
    )


def _rollback_options(plan: InstallationPlan, *, recommended: bool) -> tuple[FinalDecisionOption, ...]:
    kinds = {item.kind for item in plan.backup_actions}
    if plan.mode is InstallMode.FRESH:
        label = "Restore immediate pre-install state"
        reason = "reverse only this installation transaction using its journaled preimages"
    else:
        label = "Restore previous Realmheart version"
        reason = "reverse only this installation transaction using its journaled preimages"

    options: list[FinalDecisionOption] = [
        FinalDecisionOption(FinalAction.RESTORE_PREVIOUS, label, recommended, True, reason)
    ]
    if BackupKind.PRESERVE_EXISTING_BASELINE in kinds:
        options.append(FinalDecisionOption(
            FinalAction.RESTORE_BASELINE,
            "Restore permanent pre-Realmheart baseline",
            False,
            True,
            "after reversing this transaction, replace managed state with the validated permanent baseline",
        ))
    return tuple(options)
