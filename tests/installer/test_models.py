from __future__ import annotations

import unittest
from datetime import datetime, timezone
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.models import (
    ComponentState,
    TransactionRecord,
    TransactionState,
    to_jsonable,
)


class ModelTests(unittest.TestCase):
    def test_models_serialize_cleanly(self) -> None:
        record = TransactionRecord.create(
            schema_version=1,
            transaction_id="RH-TEST",
            installer_version="test",
            source_root=Path("/source"),
            dry_run=True,
        )
        payload = to_jsonable(record)
        self.assertEqual(payload["state"], "created")
        self.assertEqual(payload["source_root"], "/source")
        self.assertTrue(payload["dry_run"])

    def test_component_blocked_vocab_is_stable(self) -> None:
        self.assertEqual(ComponentState.BLOCKED.value, "blocked")

    def test_transaction_transition_updates_state(self) -> None:
        record = TransactionRecord(
            schema_version=1,
            transaction_id="RH-TEST",
            installer_version="test",
            state=TransactionState.CREATED,
            created_at=datetime.now(timezone.utc),
            updated_at=datetime(2000, 1, 1, tzinfo=timezone.utc),
        )
        record.transition(TransactionState.PREFLIGHT)
        self.assertEqual(record.state, TransactionState.PREFLIGHT)
        self.assertGreater(record.updated_at.year, 2000)


if __name__ == "__main__":
    unittest.main()
