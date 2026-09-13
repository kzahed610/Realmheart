from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.errors import JournalCorruptError
from realmheart_installer.models import OperationState
from realmheart_installer.transaction.journal import WriteAheadJournal, read_journal
from realmheart_installer.transaction.recovery import inspect_journal


class JournalTests(unittest.TestCase):
    def test_sequence_and_recovery_state(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "journal.jsonl"
            journal = WriteAheadJournal(path)
            journal.append(operation_id="op-a", state=OperationState.INTENT, kind="write_file", target="/fake/a")
            journal.append(operation_id="op-a", state=OperationState.STARTED, kind="write_file", target="/fake/a")
            events = read_journal(path)
            self.assertEqual([event.seq for event in events], [1, 2])
            inspection = inspect_journal(path)
            self.assertEqual(len(inspection.incomplete_operations), 1)
            self.assertEqual(inspection.incomplete_operations[0].latest_state, OperationState.STARTED)

    def test_failed_operation_requires_recovery_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "journal.jsonl"
            journal = WriteAheadJournal(path)
            for state in (OperationState.INTENT, OperationState.STARTED, OperationState.FAILED):
                journal.append(operation_id="op-a", state=state, kind="write_file", target="/fake/a")
            inspection = inspect_journal(path)
            self.assertFalse(inspection.is_clean)
            self.assertEqual(inspection.incomplete_operations[0].latest_state, OperationState.FAILED)

    def test_completed_operation_is_clean(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "journal.jsonl"
            journal = WriteAheadJournal(path)
            for state in (OperationState.INTENT, OperationState.STARTED, OperationState.COMPLETED):
                journal.append(operation_id="op-a", state=state, kind="write_file", target="/fake/a")
            self.assertTrue(inspect_journal(path).is_clean)

    def test_truncated_final_record_is_tolerated(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "journal.jsonl"
            journal = WriteAheadJournal(path)
            journal.append(operation_id="op-a", state=OperationState.INTENT)
            with path.open("ab") as handle:
                handle.write(b'{"schema_version":1,"seq":2')
            events = read_journal(path)
            self.assertEqual(len(events), 1)

    def test_malformed_complete_record_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "journal.jsonl"
            path.write_bytes(b'{"not":"valid-event"}\n')
            with self.assertRaises(JournalCorruptError):
                read_journal(path)


if __name__ == "__main__":
    unittest.main()
