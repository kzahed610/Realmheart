from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.errors import ManagedBlockConflictError
from realmheart_installer.filesystem.managed_block import ManagedBlock, plan_ensure_managed_block, remove_managed_block_bytes
from realmheart_installer.transaction.journal import WriteAheadJournal
from realmheart_installer.transaction.operations import TransactionExecutor


BLOCK = ManagedBlock(
    begin_marker="# BEGIN Realmheart Terminal Theme (managed)",
    end_marker="# END Realmheart Terminal Theme (managed)",
    body="include /fake/config/kitty/realmheart-theme.conf",
)


class ManagedBlockTests(unittest.TestCase):
    def test_insert_preserves_existing_user_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty/kitty.conf"
            target.parent.mkdir()
            original = b"font_size 12\nmap ctrl+t new_tab\n"
            target.write_bytes(original)
            preimages = root / "preimages"
            op = plan_ensure_managed_block(
                target=target,
                allowed_root=root,
                preimage_dir=preimages,
                block=BLOCK,
            )
            self.assertIsNotNone(op)
            executor = TransactionExecutor(WriteAheadJournal(root / "journal.jsonl"))
            executor.execute(op)
            result = target.read_bytes()
            self.assertTrue(result.startswith(original))
            self.assertEqual(result.count(BLOCK.begin_marker.encode()), 1)
            executor.rollback(op)
            self.assertEqual(target.read_bytes(), original)

    def test_remove_preserves_unrelated_current_bytes(self) -> None:
        block = ManagedBlock("# BEGIN RH", "# END RH", "include /realmheart")
        current = b"before\n# BEGIN RH\ninclude /realmheart\n# END RH\nafter\n"
        desired, changed = remove_managed_block_bytes(current, block)
        self.assertTrue(changed)
        self.assertEqual(desired, b"before\nafter\n")

    def test_remove_missing_block_is_idempotent_and_partial_markers_fail_closed(self) -> None:
        block = ManagedBlock("# BEGIN RH", "# END RH", "include /realmheart")
        current = b"user-only\n"
        self.assertEqual(remove_managed_block_bytes(current, block), (current, False))
        with self.assertRaises(ManagedBlockConflictError):
            remove_managed_block_bytes(b"user\n# BEGIN RH\n", block)

    def test_second_plan_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty.conf"
            target.write_bytes(BLOCK.encoded())
            op = plan_ensure_managed_block(
                target=target,
                allowed_root=root,
                preimage_dir=root / "preimages",
                block=BLOCK,
            )
            self.assertIsNone(op)

    def test_existing_valid_block_is_replaced_without_touching_neighbors(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty.conf"
            target.write_text(
                "before=exact\n"
                "# BEGIN Realmheart Terminal Theme (managed)\n"
                "include /old/path\n"
                "# END Realmheart Terminal Theme (managed)\n"
                "after=exact\n"
            )
            op = plan_ensure_managed_block(
                target=target,
                allowed_root=root,
                preimage_dir=root / "preimages",
                block=BLOCK,
            )
            self.assertIsNotNone(op)
            TransactionExecutor(WriteAheadJournal(root / "journal.jsonl")).execute(op)
            result = target.read_text()
            self.assertTrue(result.startswith("before=exact\n"))
            self.assertTrue(result.endswith("after=exact\n"))
            self.assertIn("/fake/config/kitty/realmheart-theme.conf", result)

    def test_duplicate_or_partial_markers_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / "kitty.conf"
            target.write_text(
                f"{BLOCK.begin_marker}\nold\n{BLOCK.end_marker}\n"
                f"{BLOCK.begin_marker}\nother\n{BLOCK.end_marker}\n"
            )
            with self.assertRaises(ManagedBlockConflictError):
                plan_ensure_managed_block(
                    target=target,
                    allowed_root=root,
                    preimage_dir=root / "preimages",
                    block=BLOCK,
                )


if __name__ == "__main__":
    unittest.main()
