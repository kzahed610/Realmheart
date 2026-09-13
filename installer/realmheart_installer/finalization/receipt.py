"""Transactional durable installed-state receipt publication."""
from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from ..context import InstallContext, XdgPaths
from ..filesystem.compare import fingerprint_path
from ..models import MutationPrecondition, OperationSafety, OperationState, Reversibility, to_jsonable
from ..planning.models import InstallationPlan
from ..transaction.journal import WriteAheadJournal, read_journal
from ..transaction.operations import RemovePathOperation, TransactionExecutor, WriteFileOperation, new_operation_id
from ..transaction.preconditions import FINGERPRINT_PRECONDITION, capture_path_precondition
from ..verification.models import VerificationReport


def build_installed_state_receipt(plan: InstallationPlan, report: VerificationReport, *, doctor_assessment=None) -> dict[str, object]:
    receipt = report.receipt_inputs
    if report.transaction_id != plan.transaction_id or receipt.transaction_id != plan.transaction_id:
        raise ValueError("receipt/verification transaction identity does not match final InstallationPlan")
    if report.manifest_digest != plan.manifest_digest or receipt.manifest_set_sha256 != plan.manifest_digest:
        raise ValueError("receipt/verification manifest identity does not match final InstallationPlan")
    if report.plan_digest != plan.plan_digest:
        raise ValueError("verification plan identity does not match final InstallationPlan")
    if report.activation.state.value == "pending_session_restart" and report.activation.runtime_health.value != "unknown":
        raise ValueError("pending_session_restart receipt cannot claim runtime known-good")
    components = {
        item.component_id: {
            "display_name": item.display_name,
            "category": item.category,
            "health": item.state.value,
            "blocked_by": list(item.blocked_by),
            "warnings": list(item.warnings),
            "artifact_ids": list(item.artifact_ids),
            "build_unit_ids": list(item.build_unit_ids),
        }
        for item in receipt.components
    }
    dependencies = {
        item.capability_id: {
            "component_id": item.component_id,
            "requirement": item.requirement,
            "lifecycle": list(item.lifecycle),
            "state": item.state,
            "version": item.version,
            "executable": item.executable,
            "observed_during_verification": item.observed_during_verification,
        }
        for item in receipt.dependencies
    }
    artifacts = {
        item.artifact_id: {
            "component_id": item.component_id,
            "path": item.path,
            "type": item.artifact_type,
            "ownership": item.ownership,
            "mode": item.mode,
            "uid": item.uid,
            "gid": item.gid,
            "size_bytes": item.size_bytes,
            "sha256": item.sha256,
            "immutable_fingerprint": item.immutable_fingerprint,
        }
        for item in receipt.artifacts
    }
    build_units = {
        item.build_unit_id: {
            "component_ids": list(item.component_ids),
            "health": item.health,
            "artifact_ids": list(item.artifact_ids),
            "artifact_sha256": dict(item.artifact_sha256),
            "abi_sensitive_dependencies": list(item.abi_sensitive_dependencies),
        }
        for item in receipt.build_units
    }
    return {
        "schema_version": receipt.schema_version,
        "realmheart_version": receipt.realmheart_version,
        "manifest_schema_version": receipt.manifest_schema_version,
        "manifest_set_sha256": receipt.manifest_set_sha256,
        "installer_version": receipt.installer_version,
        "transaction_id": receipt.transaction_id,
        "install_mode": plan.mode.value,
        "installation_origin": plan.environment.installation.origin.value,
        "disposition": "kept",
        "install_health": receipt.install_health,
        "activation_state": receipt.activation_state,
        "runtime_health": receipt.runtime_health,
        "verified_at": receipt.verified_at,
        "accepted_at": datetime.now(timezone.utc).isoformat(),
        "components": components,
        "dependencies": dependencies,
        "artifacts": artifacts,
        "build_units": build_units,
        "fx": to_jsonable(receipt.fx),
        "build_provenance": receipt.build_provenance,
        "doctor_acceptance": doctor_assessment.to_dict() if doctor_assessment is not None else None,
    }


def build_candidate_install_bundle(plan: InstallationPlan, report: VerificationReport) -> dict[str, object]:
    """Return the pre-acceptance Doctor handoff without claiming the state was kept.

    The shape deliberately mirrors the durable receipt fields Doctor needs, but
    has its own schema/kind and omits ``accepted_at``/``disposition``.
    """
    payload = build_installed_state_receipt(plan, report)
    payload.pop("accepted_at", None)
    payload.pop("disposition", None)
    payload.pop("doctor_acceptance", None)
    payload["schema_version"] = 1
    payload["kind"] = "realmheart_install_candidate"
    return payload


def publish_installed_state_receipt(*, plan: InstallationPlan, report: VerificationReport, context: InstallContext, paths: XdgPaths, doctor_assessment=None) -> tuple[Path, WriteFileOperation]:
    target = paths.realmheart_state / "installed-state.json"
    payload = build_installed_state_receipt(plan, report, doctor_assessment=doctor_assessment)
    encoded = (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")
    op_id = new_operation_id()
    preimage = context.preimage_dir / "receipt" / f"{op_id}.preimage"
    op = WriteFileOperation(
        target=target,
        allowed_root=paths.state_home,
        safety=OperationSafety(
            Reversibility.EXACT,
            MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), target.exists() or target.is_symlink()),
            str(preimage),
        ),
        operation_id=op_id,
        content=encoded,
        mode=0o600,
        preimage_path=preimage,
    )
    journal = WriteAheadJournal(context.journal_path)
    executor = TransactionExecutor(journal)
    try:
        executor.execute(op)
    except Exception as exc:
        # A durable receipt is the KEEP commit boundary.  If writing the receipt
        # reached the filesystem but the final journal record failed, restore the
        # exact previous receipt before surfacing the failure.  Normal
        # compare-before-write failures append FAILED and never reach apply, so
        # they are intentionally left untouched here.
        try:
            events = [event for event in read_journal(context.journal_path) if event.operation_id == op.operation_id]
            states = {event.state for event in events}
            uncertain_after_apply = (
                OperationState.STARTED in states
                and OperationState.FAILED not in states
                and OperationState.COMPLETED not in states
            )
            expected_after = op.journal_data()["expected_after_fingerprint"]
            if uncertain_after_apply and fingerprint_path(target) == expected_after:
                executor.rollback(op)
        except Exception as rollback_exc:
            raise RuntimeError(
                f"installed-state receipt commit failed and previous receipt restoration also failed: {rollback_exc}"
            ) from exc
        raise
    return target, op


def retire_installed_state_receipt_for_baseline(*, context: InstallContext, paths: XdgPaths) -> Path | None:
    """Remove the authoritative managed-install receipt after baseline restore.

    The previous receipt is moved into the transaction preimage area rather than
    destroyed.  A permanent baseline represents the pre-Realmheart state, so
    leaving a managed receipt authoritative would make the next preflight claim
    Realmheart is still installed even though its baseline was restored.
    """

    target = paths.realmheart_state / "installed-state.json"
    if not (target.exists() or target.is_symlink()):
        return None
    op_id = new_operation_id()
    backup_dir = context.preimage_dir / "baseline-receipt"
    backup_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    backup = backup_dir / f"{op_id}.previous-installed-state.json"
    op = RemovePathOperation(
        target=target,
        allowed_root=paths.state_home,
        safety=OperationSafety(
            Reversibility.EXACT,
            capture_path_precondition(target),
            str(backup),
        ),
        operation_id=op_id,
        backup_path=backup,
    )
    TransactionExecutor(WriteAheadJournal(context.journal_path)).execute(op)
    return backup
