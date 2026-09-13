from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.models import OperationSafety, Reversibility
from realmheart_installer.transaction.journal import WriteAheadJournal
from realmheart_installer.transaction.operations import MovePathOperation, RemovePathOperation, TransactionExecutor, WriteFileOperation
from realmheart_installer.transaction.preconditions import capture_path_precondition
from realmheart_installer.transaction.recovery import (
    RecoveryDisposition,
    classify_recovered_operation,
    inspect_journal,
    rollback_recovered_operation,
)


class CrashAfterApplyMove(MovePathOperation):
    def apply(self) -> None:
        super().apply()
        raise SystemExit(88)


class CrashAfterApplyRemove(RemovePathOperation):
    def apply(self) -> None:
        super().apply()
        raise SystemExit(88)


class CrashAfterApplyWrite(WriteFileOperation):
    def apply(self) -> None:
        super().apply()
        raise SystemExit(88)


class RecoveryTests(unittest.TestCase):
    def test_move_can_be_rolled_back_from_wal_after_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "active"
            source.mkdir()
            (source / "payload").write_text("old")
            destination = root / "old"
            path = root / "journal.jsonl"
            op = CrashAfterApplyMove(
                target=source,
                destination=destination,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(source)),
            )
            with self.assertRaises(SystemExit):
                TransactionExecutor(WriteAheadJournal(path)).execute(op)

            recovered = inspect_journal(path).incomplete_operations[0]
            self.assertEqual(classify_recovered_operation(recovered), RecoveryDisposition.APPLIED)
            rollback_recovered_operation(recovered, approved_roots=[root], journal=WriteAheadJournal(path))
            self.assertEqual((source / "payload").read_text(), "old")
            self.assertFalse(destination.exists())

    def test_remove_can_be_rolled_back_from_wal_after_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "managed"
            target.write_text("old")
            backup = root / ".removed"
            path = root / "journal.jsonl"
            op = CrashAfterApplyRemove(
                target=target,
                backup_path=backup,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target)),
            )
            with self.assertRaises(SystemExit):
                TransactionExecutor(WriteAheadJournal(path)).execute(op)

            recovered = inspect_journal(path).incomplete_operations[0]
            rollback_recovered_operation(recovered, approved_roots=[root], journal=WriteAheadJournal(path))
            self.assertEqual(target.read_text(), "old")
            self.assertFalse(backup.exists())

    def test_write_can_restore_preimage_from_wal_after_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty.conf"
            target.write_text("user\n")
            preimage = root / "preimages/kitty.conf"
            path = root / "journal.jsonl"
            op = CrashAfterApplyWrite(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(preimage)),
                content=b"realmheart\n",
                preimage_path=preimage,
            )
            with self.assertRaises(SystemExit):
                TransactionExecutor(WriteAheadJournal(path)).execute(op)
            self.assertEqual(target.read_text(), "realmheart\n")

            recovered = inspect_journal(path).incomplete_operations[0]
            self.assertEqual(classify_recovered_operation(recovered), RecoveryDisposition.APPLIED)
            rollback_recovered_operation(recovered, approved_roots=[root], journal=WriteAheadJournal(path))
            self.assertEqual(target.read_text(), "user\n")

    def test_external_drift_after_crash_blocks_automatic_recovery(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty.conf"
            target.write_text("user\n")
            preimage = root / "preimage"
            path = root / "journal.jsonl"
            op = CrashAfterApplyWrite(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(preimage)),
                content=b"realmheart\n",
                preimage_path=preimage,
            )
            with self.assertRaises(SystemExit):
                TransactionExecutor(WriteAheadJournal(path)).execute(op)
            target.write_text("user edited after crash\n")

            recovered = inspect_journal(path).incomplete_operations[0]
            self.assertEqual(classify_recovered_operation(recovered), RecoveryDisposition.AMBIGUOUS)
            with self.assertRaises(Exception):
                rollback_recovered_operation(recovered, approved_roots=[root], journal=WriteAheadJournal(path))
            self.assertEqual(target.read_text(), "user edited after crash\n")


if __name__ == "__main__":
    unittest.main()
