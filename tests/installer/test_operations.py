from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.errors import PreconditionFailedError, UnsafePathError
from realmheart_installer.models import OperationSafety, Reversibility
from realmheart_installer.transaction.journal import WriteAheadJournal, read_journal
from realmheart_installer.transaction.recovery import (
    RecoveryDisposition,
    classify_recovered_operation,
    inspect_journal,
    rollback_recovered_operation,
)
from realmheart_installer.transaction.operations import (
    CreateDirectoryOperation,
    MovePathOperation,
    RemovePathOperation,
    TransactionExecutor,
    WriteFileOperation,
)
from realmheart_installer.transaction.preconditions import capture_path_precondition


class DieBeforeMutationOperation(CreateDirectoryOperation):
    def apply(self) -> None:
        raise SystemExit(99)


class DieAfterMutationOperation(CreateDirectoryOperation):
    def apply(self) -> None:
        super().apply()
        raise SystemExit(99)


class OperationTests(unittest.TestCase):
    def test_abrupt_exit_after_started_leaves_recoverable_wal(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "never-created"
            journal_path = root / "journal.jsonl"
            op = DieBeforeMutationOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT),
            )
            with self.assertRaises(SystemExit):
                TransactionExecutor(WriteAheadJournal(journal_path)).execute(op)
            self.assertFalse(target.exists())
            inspection = inspect_journal(journal_path)
            self.assertEqual(inspection.incomplete_operations[0].latest_state.value, "started")

    def test_abrupt_exit_after_mutation_before_completed_is_detectable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "created-before-crash"
            journal_path = root / "journal.jsonl"
            op = DieAfterMutationOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT),
            )
            with self.assertRaises(SystemExit):
                TransactionExecutor(WriteAheadJournal(journal_path)).execute(op)
            self.assertTrue(target.exists())
            inspection = inspect_journal(journal_path)
            self.assertEqual(inspection.incomplete_operations[0].latest_state.value, "started")
            recovered = inspection.incomplete_operations[0]
            self.assertEqual(classify_recovered_operation(recovered), RecoveryDisposition.APPLIED)
            # Discarding the original operation object models a fresh process:
            # rollback is reconstructed from durable intent metadata only.
            rollback_recovered_operation(
                recovered,
                approved_roots=[root],
                journal=WriteAheadJournal(journal_path),
            )
            self.assertFalse(target.exists())
            self.assertTrue(inspect_journal(journal_path).is_clean)

    def test_write_file_and_exact_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "config/kitty/kitty.conf"
            target.parent.mkdir(parents=True)
            target.write_text("user config\n")
            precondition = capture_path_precondition(target)
            preimage = root / "preimages/kitty.conf"
            journal = WriteAheadJournal(root / "journal.jsonl")
            executor = TransactionExecutor(journal)
            op = WriteFileOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, precondition, str(preimage)),
                content=b"user config\n# realmheart\n",
                mode=0o600,
                preimage_path=preimage,
            )
            executor.execute(op)
            self.assertIn("realmheart", target.read_text())
            executor.rollback(op)
            self.assertEqual(target.read_text(), "user config\n")
            states = [event.state.value for event in read_journal(root / "journal.jsonl")]
            self.assertEqual(
                states,
                ["intent", "started", "completed", "rollback_started", "rolled_back"],
            )

    def test_write_restores_preexisting_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "config/realmheart.conf"
            target.parent.mkdir(parents=True)
            original_target = root / "personal.conf"
            original_target.write_text("personal")
            os.symlink(original_target, target)
            precondition = capture_path_precondition(target)
            op = WriteFileOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, precondition, str(root / "preimage")),
                content=b"managed\n",
                preimage_path=root / "preimage",
            )
            executor = TransactionExecutor(WriteAheadJournal(root / "journal.jsonl"))
            executor.execute(op)
            self.assertFalse(target.is_symlink())
            executor.rollback(op)
            self.assertTrue(target.is_symlink())
            self.assertEqual(os.readlink(target), str(original_target))

    def test_precondition_drift_prevents_write(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty.conf"
            target.write_text("planned\n")
            precondition = capture_path_precondition(target)
            target.write_text("changed by user\n")
            op = WriteFileOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, precondition, str(root / "preimage")),
                content=b"realmheart\n",
                preimage_path=root / "preimage",
            )
            executor = TransactionExecutor(WriteAheadJournal(root / "journal.jsonl"))
            with self.assertRaises(PreconditionFailedError):
                executor.execute(op)
            self.assertEqual(target.read_text(), "changed by user\n")
            self.assertEqual(read_journal(root / "journal.jsonl"), [])

    def test_create_directory_rolls_back_only_own_empty_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "new-dir"
            op = CreateDirectoryOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT),
            )
            executor = TransactionExecutor(WriteAheadJournal(root / "journal.jsonl"))
            executor.execute(op)
            self.assertTrue(target.is_dir())
            executor.rollback(op)
            self.assertFalse(target.exists())

    def test_move_and_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "active"
            source.mkdir()
            (source / "file").write_text("x")
            destination = root / "old"
            op = MovePathOperation(
                target=source,
                destination=destination,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(source)),
            )
            executor = TransactionExecutor(WriteAheadJournal(root / "journal.jsonl"))
            executor.execute(op)
            self.assertTrue((destination / "file").exists())
            executor.rollback(op)
            self.assertTrue((source / "file").exists())

    def test_remove_is_rename_backed_and_reversible(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "managed-file"
            target.write_text("managed")
            backup = root / ".removed-op"
            op = RemovePathOperation(
                target=target,
                backup_path=backup,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target)),
            )
            executor = TransactionExecutor(WriteAheadJournal(root / "journal.jsonl"))
            executor.execute(op)
            self.assertFalse(target.exists())
            self.assertTrue(backup.exists())
            executor.rollback(op)
            self.assertEqual(target.read_text(), "managed")

    def test_path_escape_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root.parent / "outside"
            op = CreateDirectoryOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT),
            )
            with self.assertRaises(UnsafePathError):
                TransactionExecutor(WriteAheadJournal(root / "journal.jsonl")).execute(op)


if __name__ == "__main__":
    unittest.main()
