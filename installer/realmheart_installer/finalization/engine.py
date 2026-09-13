"""Apply the final keep/rollback decision after observed verification."""
from __future__ import annotations

from ..context import InstallContext, XdgPaths
from ..models import TransactionState
from ..planning.models import InstallationPlan
from ..verification.models import InstallHealthState, VerificationReport
from .decision import build_final_decision
from .models import FinalAction, FinalizationResult, RollbackStatus
from .receipt import publish_installed_state_receipt, retire_installed_state_receipt_for_baseline


class FinalizationEngine:
    def __init__(self, *, plan: InstallationPlan, verification: VerificationReport, context: InstallContext, paths: XdgPaths, backend, doctor_assessment=None) -> None:
        self.plan = plan
        self.verification = verification
        self.context = context
        self.paths = paths
        self.backend = backend
        self.doctor_assessment = doctor_assessment

    def decision_plan(self):
        return build_final_decision(self.plan, self.verification, self.doctor_assessment)

    def apply(self, action: FinalAction) -> FinalizationResult:
        decision = self.decision_plan()
        allowed = {item.action for item in decision.options}
        if action not in allowed:
            raise ValueError(f"final action {action.value} is not valid for observed state")
        self.context.transaction.transition(TransactionState.DECISION_REQUIRED)
        self.context.transaction.metadata["final_decision"] = action.value
        self.context.persist_summary()

        if action is FinalAction.KEEP:
            self.context.transaction.transition(TransactionState.COMMITTING)
            self.context.persist_summary()
            receipt_path = None
            receipt_op = None
            try:
                receipt_path, receipt_op = publish_installed_state_receipt(
                    plan=self.plan, report=self.verification, context=self.context, paths=self.paths, doctor_assessment=self.doctor_assessment,
                )
            except Exception:
                # The receipt is part of the same decision boundary. If KEEP
                # cannot be made durable, restore its old preimage and leave the
                # live transaction available for rollback/recovery.
                if receipt_op is not None:
                    from ..transaction.journal import WriteAheadJournal
                    from ..transaction.operations import TransactionExecutor
                    TransactionExecutor(WriteAheadJournal(self.context.journal_path)).rollback(receipt_op)
                self.context.transaction.transition(TransactionState.FAILED)
                self.context.persist_summary()
                raise
            self.context.transaction.transition(TransactionState.COMMITTED)
            self.context.transaction.metadata["receipt_path"] = str(receipt_path)
            self.context.persist_summary()
            # Retained transaction-old siblings are cleanup material only after
            # the durable receipt exists. Cleanup errors are warnings, not a
            # reason to pretend the accepted install was never committed.
            finalize_errors = tuple(self.backend.finalize_keep())
            failed_keep = self.verification.install_health is InstallHealthState.FAILED
            warnings = tuple(self.verification.warnings) + tuple(f"post-keep cleanup: {item}" for item in finalize_errors)
            return FinalizationResult(
                action, "kept", 20 if failed_keep else 0, str(receipt_path),
                RollbackStatus(False, True, None, (), True),
                warnings,
                "failed installation explicitly kept" if failed_keep else "installation kept and durable receipt committed",
            )

        # RESTORE_PREVIOUS is exactly this transaction's reverse operation set.
        # The previous installed-state receipt is deliberately not rewritten.
        self.context.transaction.transition(TransactionState.ROLLING_BACK)
        self.context.persist_summary()
        errors = tuple(self.backend.rollback_all())
        baseline_receipt_backup = None
        if action is FinalAction.RESTORE_BASELINE and not errors:
            errors = tuple(self.backend.restore_permanent_baseline(self.paths.baseline_backup))
            if not errors:
                try:
                    baseline_receipt_backup = retire_installed_state_receipt_for_baseline(
                        context=self.context, paths=self.paths,
                    )
                except Exception as exc:
                    errors = (f"restored baseline but could not retire managed installed-state receipt: {exc}",)
        package_warning = ()
        if self.context.transaction.metadata.get("package_install") is not None:
            package_warning = (
                "system dependency package changes from this transaction were retained; automatic package removal is intentionally conservative",
            )
        if errors:
            self.context.transaction.transition(TransactionState.ROLLBACK_FAILED)
            self.context.transaction.metadata["rollback_errors"] = list(errors)
            self.context.persist_summary()
            return FinalizationResult(
                action, "rollback_failed", 22, None,
                RollbackStatus(True, False, action.value, errors, True), package_warning,
                "rollback completed with errors; manual attention required",
            )
        self.context.transaction.transition(TransactionState.ROLLED_BACK)
        self.context.persist_summary()
        return FinalizationResult(
            action, "rolled_back", 21, None,
            RollbackStatus(True, True, action.value, (), True), package_warning,
            (
                "permanent pre-Realmheart baseline restored; prior managed receipt archived in transaction preimages"
                if action is FinalAction.RESTORE_BASELINE
                else "installation attempt rolled back; previous receipt was preserved"
            ),
        )
