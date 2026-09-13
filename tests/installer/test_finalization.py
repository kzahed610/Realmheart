from __future__ import annotations

import json
import os
import tempfile
import unittest
from unittest.mock import patch
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path

from . import _bootstrap
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.context import InstallContext, XdgPaths
from realmheart_installer.constants import INSTALLER_VERSION
from realmheart_installer.finalization import FinalAction, FinalizationEngine, build_final_decision, build_installed_state_receipt
from realmheart_installer.finalization.models import FinalSeverity
from realmheart_installer.models import InstallMode, OperationState, TransactionState
from realmheart_installer.planning.models import BackupKind
from realmheart_installer.verification.models import (
    ActivationState, ActivationVerification, ComponentHealthState, ComponentVerification,
    FxReceiptInput, InstallHealthState, ReceiptInputAssembly, RuntimeHealthState, VerificationReport,
)


class FakeBackend:
    def __init__(self, *, rollback_errors=(), baseline_errors=(), finalize_errors=()):
        self.rollback_errors = tuple(rollback_errors)
        self.baseline_errors = tuple(baseline_errors)
        self.finalize_errors = tuple(finalize_errors)
        self.calls=[]
    def rollback_all(self): self.calls.append("rollback"); return self.rollback_errors
    def restore_permanent_baseline(self, path): self.calls.append(("baseline", str(path))); return self.baseline_errors
    def finalize_keep(self): self.calls.append("finalize"); return self.finalize_errors


def paths_for(root: Path) -> XdgPaths:
    env={"HOME":str(root/"home"),"XDG_CONFIG_HOME":str(root/"cfg"),"XDG_STATE_HOME":str(root/"state"),"XDG_DATA_HOME":str(root/"data"),"XDG_CACHE_HOME":str(root/"cache"),"XDG_RUNTIME_DIR":str(root/"run")}
    for value in env.values(): Path(value).mkdir(parents=True, exist_ok=True)
    return XdgPaths.resolve(env=env, uid=os.getuid())


def component(cid: str, state: ComponentHealthState, category="core") -> ComponentVerification:
    return ComponentVerification(cid, cid, category, state, (), (), (), (), ())


def verification(*, transaction_id="RH-FINAL", manifest_digest="manifest", plan_digest="plan", health=InstallHealthState.HEALTHY, activation=ActivationState.ACTIVE, runtime=RuntimeHealthState.HEALTHY, core=ComponentHealthState.HEALTHY, fx=ComponentHealthState.HEALTHY, warnings=()):
    comps=(component("realmheart-core",core), component("realmheart-fx",fx,"fx"))
    receipt=ReceiptInputAssembly(
        2,"0.7.8",1,manifest_digest,INSTALLER_VERSION,transaction_id,health.value,activation.value,runtime.value,
        datetime.now(timezone.utc).isoformat(),comps,(),(),(),
        FxReceiptInput(True,"compatible","realmheart-fx","build","fx.plugin","fx.loader",None,None,"0.56.2","abc","abi",()),
        {"compiler":"test"},
    )
    return VerificationReport(1,transaction_id,"0.7.8",manifest_digest,plan_digest,health,ActivationVerification(activation,runtime,"test",True,True),comps,(),(),(),receipt,tuple(warnings),())


class Phase16FinalizationTests(unittest.TestCase):
    def _plan(self, root: Path, mode=InstallMode.FRESH):
        # Reuse a fully valid planned object from the existing Phase-13 fixture,
        # then change only the fields Phase 16 needs to vary.
        from tests.installer.test_verification_engine import Phase13VerificationTests
        helper=Phase13VerificationTests(); helper.setUp()
        paths, runner, plan, build = helper._fixture(root)
        return paths, replace(plan, mode=mode)

    def test_healthy_install_auto_keeps(self):
        with tempfile.TemporaryDirectory() as temp:
            paths, plan=self._plan(Path(temp))
            decision=build_final_decision(plan, verification())
            self.assertEqual(decision.severity, FinalSeverity.SUCCESS)
            self.assertEqual(decision.default_action, FinalAction.KEEP)
            self.assertFalse(decision.requires_explicit_choice)

    def test_pending_activation_receipt_never_claims_runtime_known_good(self):
        with tempfile.TemporaryDirectory() as temp:
            paths, plan=self._plan(Path(temp))
            report=verification(transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest, activation=ActivationState.PENDING_SESSION_RESTART, runtime=RuntimeHealthState.UNKNOWN)
            receipt=build_installed_state_receipt(plan, report)
            self.assertEqual(receipt["activation_state"], "pending_session_restart")
            self.assertEqual(receipt["runtime_health"], "unknown")

    def test_degraded_install_requires_explicit_choice(self):
        with tempfile.TemporaryDirectory() as temp:
            _, plan=self._plan(Path(temp))
            decision=build_final_decision(plan, verification(health=InstallHealthState.DEGRADED, runtime=RuntimeHealthState.DEGRADED, warnings=("qol failed",)))
            self.assertTrue(decision.requires_explicit_choice)
            self.assertIsNone(decision.default_action)
            self.assertIn(FinalAction.KEEP,{x.action for x in decision.options})

    def test_required_fx_failure_is_core_critical_and_rollback_preferred(self):
        with tempfile.TemporaryDirectory() as temp:
            _, plan=self._plan(Path(temp), InstallMode.UPGRADE)
            decision=build_final_decision(plan, verification(health=InstallHealthState.FAILED, runtime=RuntimeHealthState.FAILED, core=ComponentHealthState.FAILED, fx=ComponentHealthState.FAILED))
            self.assertTrue(decision.fx_critical)
            self.assertTrue(decision.core_critical)
            self.assertEqual(decision.options[0].action, FinalAction.RESTORE_PREVIOUS)


    def test_fresh_install_with_existing_baseline_separates_immediate_rollback_from_baseline_restore(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root, InstallMode.FRESH)
            preserved = replace(
                plan.backup_actions[0],
                id="backup.baseline.preserve",
                kind=BackupKind.PRESERVE_EXISTING_BASELINE,
                destination=str(paths.baseline_backup),
                already_exists=True,
                targets=(),
            )
            plan=replace(plan, backup_actions=(preserved,))
            decision=build_final_decision(
                plan,
                verification(
                    transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest,
                    health=InstallHealthState.FAILED, runtime=RuntimeHealthState.FAILED, core=ComponentHealthState.FAILED,
                ),
            )
            self.assertEqual(decision.options[0].action, FinalAction.RESTORE_PREVIOUS)
            self.assertIn("immediate pre-install", decision.options[0].label.lower())
            self.assertEqual(decision.options[1].action, FinalAction.RESTORE_BASELINE)
            self.assertTrue(decision.options[0].recommended)
            self.assertFalse(decision.options[1].recommended)

    def test_degraded_keep_is_the_only_recommended_choice(self):
        with tempfile.TemporaryDirectory() as temp:
            _, plan=self._plan(Path(temp))
            decision=build_final_decision(
                plan,
                verification(health=InstallHealthState.DEGRADED, runtime=RuntimeHealthState.DEGRADED, warnings=("qol failed",)),
            )
            recommended=[item.action for item in decision.options if item.recommended]
            self.assertEqual(recommended, [FinalAction.KEEP])

    def test_restore_baseline_runs_only_after_transaction_rollback(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root, InstallMode.UPGRADE)
            preserved = replace(
                plan.backup_actions[0],
                id="backup.baseline.preserve",
                kind=BackupKind.PRESERVE_EXISTING_BASELINE,
                destination=str(paths.baseline_backup),
                already_exists=True,
                targets=(),
            )
            previous = replace(
                plan.backup_actions[-1],
                id="backup.previous-version",
                kind=BackupKind.PREVIOUS_VERSION,
            )
            plan=replace(plan, backup_actions=(preserved, previous))
            context=InstallContext.create(paths=paths,source_root=_bootstrap.REPO_ROOT,transaction_id=plan.transaction_id)
            backend=FakeBackend()
            result=FinalizationEngine(
                plan=plan,
                verification=verification(
                    transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest,
                    health=InstallHealthState.FAILED, runtime=RuntimeHealthState.FAILED, core=ComponentHealthState.FAILED,
                ),
                context=context, paths=paths, backend=backend,
            ).apply(FinalAction.RESTORE_BASELINE)
            self.assertEqual(result.exit_code,21)
            self.assertEqual(backend.calls[0], "rollback")
            self.assertEqual(backend.calls[1], ("baseline", str(paths.baseline_backup)))

    def test_rollback_preserves_previous_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root, InstallMode.UPGRADE)
            context=InstallContext.create(paths=paths, source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            receipt_path=paths.realmheart_state/"installed-state.json"; receipt_path.parent.mkdir(parents=True,exist_ok=True)
            receipt_path.write_text('{"schema_version":2,"realmheart_version":"0.7.7","disposition":"kept"}\n')
            before=receipt_path.read_bytes()
            result=FinalizationEngine(plan=plan,verification=verification(transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest, health=InstallHealthState.FAILED, runtime=RuntimeHealthState.FAILED, core=ComponentHealthState.FAILED),context=context,paths=paths,backend=FakeBackend()).apply(FinalAction.RESTORE_PREVIOUS)
            self.assertEqual(result.exit_code,21)
            self.assertEqual(receipt_path.read_bytes(),before)

    def test_kept_failed_install_gets_honest_receipt_and_exit_20(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root)
            context=InstallContext.create(paths=paths, source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            report=verification(transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest, health=InstallHealthState.FAILED,runtime=RuntimeHealthState.FAILED,core=ComponentHealthState.FAILED)
            result=FinalizationEngine(plan=plan,verification=report,context=context,paths=paths,backend=FakeBackend()).apply(FinalAction.KEEP)
            self.assertEqual(result.exit_code,20)
            payload=json.loads((paths.realmheart_state/"installed-state.json").read_text())
            self.assertEqual(payload["disposition"],"kept")
            self.assertEqual(payload["install_health"],"failed")

    def test_receipt_replaces_previous_only_on_keep_and_is_mode_0600(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root)
            old=paths.realmheart_state/"installed-state.json"; old.parent.mkdir(parents=True,exist_ok=True); old.write_text("old\n")
            context=InstallContext.create(paths=paths, source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            FinalizationEngine(plan=plan,verification=verification(transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest),context=context,paths=paths,backend=FakeBackend()).apply(FinalAction.KEEP)
            self.assertEqual(old.stat().st_mode & 0o777,0o600)
            self.assertEqual(json.loads(old.read_text())["transaction_id"], plan.transaction_id)

    def test_restore_baseline_retires_authoritative_receipt_but_preserves_bytes_in_preimages(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root, InstallMode.UPGRADE)
            preserved = replace(
                plan.backup_actions[0], id="backup.baseline.preserve",
                kind=BackupKind.PRESERVE_EXISTING_BASELINE, destination=str(paths.baseline_backup),
                already_exists=True, targets=(),
            )
            plan=replace(plan, backup_actions=(preserved,))
            receipt_path=paths.realmheart_state/"installed-state.json"
            receipt_path.parent.mkdir(parents=True,exist_ok=True)
            old=b'{"schema_version":2,"realmheart_version":"0.7.7","disposition":"kept"}\n'
            receipt_path.write_bytes(old)
            context=InstallContext.create(paths=paths,source_root=_bootstrap.REPO_ROOT,transaction_id=plan.transaction_id)
            result=FinalizationEngine(
                plan=plan,
                verification=verification(
                    transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest,
                    health=InstallHealthState.FAILED, runtime=RuntimeHealthState.FAILED, core=ComponentHealthState.FAILED,
                ),
                context=context,paths=paths,backend=FakeBackend(),
            ).apply(FinalAction.RESTORE_BASELINE)
            self.assertEqual(result.exit_code,21)
            self.assertFalse(receipt_path.exists(),"pre-Realmheart baseline must not leave a managed receipt authoritative")
            preserved_receipts=list((context.preimage_dir/"baseline-receipt").glob("*.previous-installed-state.json"))
            self.assertEqual(len(preserved_receipts),1)
            self.assertEqual(preserved_receipts[0].read_bytes(),old)

    def test_rollback_reports_retained_package_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root,InstallMode.UPGRADE)
            context=InstallContext.create(paths=paths,source_root=_bootstrap.REPO_ROOT,transaction_id=plan.transaction_id)
            context.transaction.metadata["package_install"]={"installed":["example"]}
            result=FinalizationEngine(
                plan=plan,
                verification=verification(
                    transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest,
                    health=InstallHealthState.FAILED,runtime=RuntimeHealthState.FAILED,core=ComponentHealthState.FAILED,
                ),
                context=context,paths=paths,backend=FakeBackend(),
            ).apply(FinalAction.RESTORE_PREVIOUS)
            self.assertEqual(result.exit_code,21)
            self.assertTrue(any("package" in item for item in result.warnings))

    def test_receipt_completed_journal_failure_restores_previous_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root)
            receipt_path=paths.realmheart_state/"installed-state.json"
            receipt_path.parent.mkdir(parents=True,exist_ok=True)
            receipt_path.write_text('{"schema_version":2,"realmheart_version":"0.7.7","disposition":"kept"}\n')
            before=receipt_path.read_bytes()
            context=InstallContext.create(paths=paths,source_root=_bootstrap.REPO_ROOT,transaction_id=plan.transaction_id)
            report=verification(transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest)
            from realmheart_installer.finalization import receipt as receipt_module
            original=receipt_module.WriteAheadJournal.append

            def fail_completed(journal, **kwargs):
                if kwargs.get("state") is OperationState.COMPLETED and kwargs.get("target") == str(receipt_path):
                    raise OSError("synthetic receipt completion journal failure")
                return original(journal, **kwargs)

            with patch.object(receipt_module.WriteAheadJournal, "append", new=fail_completed):
                with self.assertRaises(OSError):
                    FinalizationEngine(plan=plan,verification=report,context=context,paths=paths,backend=FakeBackend()).apply(FinalAction.KEEP)
            self.assertEqual(receipt_path.read_bytes(),before)
            self.assertEqual(context.transaction.state,TransactionState.FAILED)

    def test_rollback_failure_is_never_reported_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths, plan=self._plan(root,InstallMode.UPGRADE)
            context=InstallContext.create(paths=paths,source_root=_bootstrap.REPO_ROOT,transaction_id=plan.transaction_id)
            result=FinalizationEngine(plan=plan,verification=verification(transaction_id=plan.transaction_id, manifest_digest=plan.manifest_digest, plan_digest=plan.plan_digest, health=InstallHealthState.FAILED,runtime=RuntimeHealthState.FAILED,core=ComponentHealthState.FAILED),context=context,paths=paths,backend=FakeBackend(rollback_errors=("restore failed",))).apply(FinalAction.RESTORE_PREVIOUS)
            self.assertEqual(result.exit_code,22)
            self.assertFalse(result.rollback.ok)
            self.assertIn("restore failed",result.rollback.errors)

if __name__ == "__main__": unittest.main()
