"""Phase-16 top-level live transaction orchestration."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from realmheart_maintenance.manifest import ManifestRegistry
from realmheart_doctor import AcceptanceAssessment, AcceptanceFinding, AcceptanceRecommendation, assess_candidate_install

from ..components.execution import execute_installation_components
from ..context import InstallContext, XdgPaths
from ..diagnostics import DiagnosticReportBuilder, DiagnosticReportStore
from ..environment.command import CommandRunner
from ..errors import InstallerError, interruption_reason
from ..filesystem.backup import create_backup_snapshot, ensure_permanent_baseline, validate_backup_snapshot
from ..finalization import FinalAction, FinalDecisionPlan, FinalizationEngine, FinalizationResult, build_candidate_install_bundle
from ..models import TransactionState, to_jsonable
from ..native_build import NativeBuildExecutor
from ..native_build.models import BuildStageReport
from ..planning.models import BackupKind, InstallationPlan
from ..verification import VerificationEngine
from ..verification.models import VerificationReport
from ..transaction.recovery import persist_recovery_report
from .backend import LiveMutationBackend

DecisionSelector = Callable[[FinalDecisionPlan], FinalAction]


@dataclass(frozen=True)
class LiveInstallResult:
    build_report: BuildStageReport
    component_report: object
    verification: VerificationReport
    doctor_assessment: AcceptanceAssessment
    decision: FinalDecisionPlan
    finalization: FinalizationResult
    diagnostic_incident_id: str | None


class LiveInstallExecutor:
    """Execute one complete planned installation through the keep/rollback boundary."""

    def __init__(
        self,
        *,
        plan: InstallationPlan,
        registry: ManifestRegistry,
        context: InstallContext,
        paths: XdgPaths,
        source_root: Path,
        runner: CommandRunner,
        snapshot,
        decision_selector: DecisionSelector,
        package_actions_applied: bool = False,
        allow_unprivileged_system_commit: bool = False,
        activate_user_services: bool = True,
        persist_diagnostic_on_failure: bool = True,
        privileged_uid: int = 0,
        privileged_gid: int = 0,
    ) -> None:
        self.plan = plan
        self.registry = registry
        self.context = context
        self.paths = paths
        self.source_root = Path(source_root)
        self.runner = runner
        self.snapshot = snapshot
        self.decision_selector = decision_selector
        self.package_actions_applied = package_actions_applied
        self.allow_unprivileged_system_commit = allow_unprivileged_system_commit
        self.activate_user_services = activate_user_services
        self.persist_diagnostic_on_failure = persist_diagnostic_on_failure
        self.privileged_uid = privileged_uid
        self.privileged_gid = privileged_gid

    def run(self) -> LiveInstallResult:
        if not self.plan.ready:
            raise InstallerError("live installation requires a READY InstallationPlan", code="RH_LIVE_PLAN_BLOCKED", stage="live_install")
        if self.plan.package_actions and not self.package_actions_applied:
            raise InstallerError(
                "planned dependency package actions must be completed and re-probed before live Realmheart mutation",
                code="RH_LIVE_PACKAGES_PENDING", stage="live_install",
            )

        self.context.ensure_recovery_reserve()
        backend: LiveMutationBackend | None = None
        live_mutation_started = False
        rollback_errors: tuple[str, ...] = ()
        try:
            self.context.transaction.transition(TransactionState.BACKUP)
            self.context.persist_summary()
            self._execute_backups()

            self.context.transaction.transition(TransactionState.APPLYING)
            self.context.transaction.metadata["live_mutation_started"] = False
            self.context.persist_summary()
            build = NativeBuildExecutor(
                plan=self.plan, source_root=self.source_root, registry=self.registry,
                runner=self.runner, installer_cache=self.paths.installer_cache,
            ).run()
            self.context.transaction.metadata["build_stage"] = to_jsonable(build)
            self.context.persist_json("build-stage.json", build)
            self.context.persist_summary()
            if not build.ok:
                self.context.transaction.transition(TransactionState.FAILED)
                self.context.persist_summary()
                raise InstallerError("native build/staged install failed before live mutation", code="RH_LIVE_BUILD_STAGE_FAILED", stage="build_stage")

            backend = LiveMutationBackend(
                plan=self.plan, build_report=build, context=self.context, paths=self.paths,
                source_root=self.source_root, runner=self.runner,
                allow_unprivileged_system_commit=self.allow_unprivileged_system_commit,
                activate_user_services=self.activate_user_services,
            )
            live_mutation_started = True
            self.context.transaction.metadata["live_mutation_started"] = True
            self.context.persist_summary()

            components = execute_installation_components(
                self.plan, self.registry, backend,
                package_actions_applied=True,
            )
            self.context.persist_json("components.json", components)

            self.context.transaction.transition(TransactionState.VERIFYING)
            self.context.persist_summary()
            verification = VerificationEngine(
                plan=self.plan, registry=self.registry, runner=self.runner,
                paths=self.paths, source_root=self.source_root,
                capability_results=self.snapshot.capabilities,
                build_report=build,
                privileged_uid=self.privileged_uid,
                privileged_gid=self.privileged_gid,
            ).run()
            self.context.persist_json("verification.json", verification)

            candidate = build_candidate_install_bundle(self.plan, verification)
            self.context.persist_json("doctor-candidate.json", candidate)
            try:
                doctor = assess_candidate_install(self.registry, candidate)
            except Exception as exc:
                # Doctor is intentionally a second opinion, never a new single
                # point of failure for an otherwise verified installation.
                doctor = AcceptanceAssessment(
                    schema_version=1,
                    recommendation=AcceptanceRecommendation.INDETERMINATE,
                    transaction_id=self.plan.transaction_id,
                    realmheart_version=self.plan.target_version,
                    manifest_digest=self.plan.manifest_digest,
                    checked_artifacts=0,
                    checked_capabilities=0,
                    activation_state=verification.activation.state.value,
                    runtime_health=verification.activation.runtime_health.value,
                    findings=(AcceptanceFinding(
                        "RH_DOCTOR_ACCEPTANCE_ERROR", "warning", "doctor",
                        f"Doctor acceptance assessment failed: {type(exc).__name__}: {exc}",
                    ),),
                    forensic_drift_count=0,
                    forensic_incident_count=0,
                    summary="Doctor could not establish an independent acceptance verdict; installer verification remains authoritative.",
                )
            self.context.persist_json("doctor-assessment.json", doctor)
            self.context.transaction.metadata["doctor_acceptance"] = doctor.to_dict()
            self.context.persist_summary()

            diagnostic = DiagnosticReportBuilder(
                paths=self.paths, source_root=self.source_root,
                transaction_id=self.plan.transaction_id,
                snapshot=self.snapshot, plan=self.plan,
                verification=verification, build_report=build,
                transaction_dir=self.context.transaction_dir,
            ).build()
            incident_id = None
            if self.persist_diagnostic_on_failure and diagnostic.has_failures:
                try:
                    bundle = DiagnosticReportStore(self.paths).save(diagnostic)
                    incident_id = bundle.incident_id
                    self.context.transaction.metadata["diagnostic_incident_id"] = incident_id
                except Exception as exc:
                    # Diagnostic persistence is deliberately not part of
                    # transaction health. The in-memory report still exists and
                    # the terminal/finalization path must continue.
                    self.context.transaction.metadata.setdefault("warnings", []).append(
                        f"diagnostic report persistence failed: {type(exc).__name__}: {exc}"
                    )
                    self.context.persist_summary()

            finalizer = FinalizationEngine(
                plan=self.plan, verification=verification, context=self.context,
                paths=self.paths, backend=backend, doctor_assessment=doctor,
            )
            decision = finalizer.decision_plan()
            self.context.persist_json("final-decision.json", decision)
            action = self.decision_selector(decision) if decision.requires_explicit_choice else decision.default_action
            if action is None:
                raise InstallerError(
                    "finalization requires an explicit decision for the observed health state",
                    code="RH_FINAL_DECISION_REQUIRED", stage="finalization",
                )
            final = finalizer.apply(action)
            self.context.persist_json("finalization.json", final)
            if action is not FinalAction.KEEP:
                persist_recovery_report(
                    self.context,
                    trigger="finalization_rollback" if final.rollback.ok else "finalization_rollback_failed",
                    rollback_errors=final.rollback.errors,
                )
            else:
                self.context.release_recovery_reserve()
            return LiveInstallResult(build, components, verification, doctor, decision, final, incident_id)

        except KeyboardInterrupt as exc:
            # SIGINT/SIGTERM deliberately do not attempt complex rollback from
            # the asynchronous interruption path.  Preserve an already-terminal
            # transaction instead of turning a durable commit into INTERRUPTED.
            reason = interruption_reason(exc)
            if self.context.transaction.state not in {
                TransactionState.COMMITTED, TransactionState.ROLLED_BACK, TransactionState.ROLLBACK_FAILED
            }:
                self.context.transaction.transition(TransactionState.INTERRUPTED)
                self.context.transaction.metadata["interruption"] = reason
            else:
                self.context.transaction.metadata["interruption_after_terminal"] = reason
            try:
                self.context.persist_summary()
            except Exception:
                pass
            persist_recovery_report(self.context, trigger="process_interruption", primary_error=exc)
            raise

        except Exception as exc:
            # Any unexpected exception after live mutation starts must not leave
            # a silent half-install. Attempt the same transaction-owned reverse
            # path used by an explicit rollback decision.  Pre-mutation failures
            # are recorded as FAILED without inventing a rollback that never ran.
            if live_mutation_started and backend is not None and self.context.transaction.state not in {
                TransactionState.COMMITTED, TransactionState.ROLLED_BACK, TransactionState.ROLLBACK_FAILED
            }:
                self.context.transaction.transition(TransactionState.ROLLING_BACK)
                try:
                    self.context.persist_summary()
                except Exception:
                    pass
                rollback_errors = tuple(backend.rollback_all())
                if rollback_errors:
                    self.context.transaction.transition(TransactionState.ROLLBACK_FAILED)
                    self.context.transaction.metadata["rollback_errors"] = list(rollback_errors)
                else:
                    self.context.transaction.transition(TransactionState.ROLLED_BACK)
            elif self.context.transaction.state not in {
                TransactionState.COMMITTED, TransactionState.ROLLED_BACK, TransactionState.ROLLBACK_FAILED
            }:
                self.context.transaction.transition(TransactionState.FAILED)
            try:
                self.context.persist_summary()
            except Exception:
                pass
            persist_recovery_report(
                self.context, trigger="live_install_exception", primary_error=exc, rollback_errors=rollback_errors,
            )
            raise

    def _execute_backups(self) -> None:
        for action in self.plan.backup_actions:
            destination = Path(action.destination)
            if action.kind is BackupKind.PRESERVE_EXISTING_BASELINE:
                validation = validate_backup_snapshot(destination)
                if not validation.valid:
                    raise InstallerError(
                        "existing permanent baseline failed validation: " + "; ".join(validation.errors),
                        code="RH_BASELINE_INVALID", stage="backup",
                    )
                continue
            sources = {target.label: Path(target.path) for target in action.targets}
            if action.kind is BackupKind.PERMANENT_BASELINE:
                ensure_permanent_baseline(
                    destination, sources,
                    installer_version=self.context.transaction.installer_version,
                    target_realmheart_version=self.plan.target_version,
                    transaction_id=self.plan.transaction_id,
                )
            else:
                create_backup_snapshot(
                    destination, sources,
                    snapshot_kind=action.kind.value,
                    installer_version=self.context.transaction.installer_version,
                    target_realmheart_version=self.plan.target_version,
                    transaction_id=self.plan.transaction_id,
                )
            self.context.transaction.metadata.setdefault("backups_created", []).append(str(destination))
            self.context.persist_summary()
