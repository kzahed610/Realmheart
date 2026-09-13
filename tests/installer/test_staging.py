from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.filesystem.staging import FullTreeSwap, prepare_full_tree_stage
from realmheart_installer.transaction.journal import WriteAheadJournal
from realmheart_installer.transaction.recovery import rollback_transaction_from_journal


class StagingTests(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[Path, Path]:
        release = root / "release-hypr"
        active = root / "config/hypr"
        (release / "custom").mkdir(parents=True)
        (release / "hyprland.conf").write_text("realmheart=v2\n")
        (release / "custom/default-existing.lua").write_text("release existing\n")
        (release / "custom/new-default.lua").write_text("new default\n")

        (active / "custom").mkdir(parents=True)
        (active / "hyprland.conf").write_text("realmheart=v1\n")
        (active / "custom/default-existing.lua").write_text("USER OVERRIDE\n")
        (active / "custom/user-only.lua").write_text("my config\n")
        return release, active

    def test_custom_island_is_preserved_and_new_defaults_are_seeded(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            release, active = self._fixture(root)
            stage = prepare_full_tree_stage(
                release_tree=release,
                target=active,
                transaction_id="RH-TEST",
                preserve_relative_paths=("custom",),
            )
            self.assertEqual((stage.staging / "hyprland.conf").read_text(), "realmheart=v2\n")
            self.assertEqual((stage.staging / "custom/default-existing.lua").read_text(), "USER OVERRIDE\n")
            self.assertEqual((stage.staging / "custom/user-only.lua").read_text(), "my config\n")
            self.assertEqual((stage.staging / "custom/new-default.lua").read_text(), "new default\n")

    def test_swap_is_exactly_rollbackable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            release, active = self._fixture(root)
            stage = prepare_full_tree_stage(release_tree=release, target=active, transaction_id="RH-TEST")
            journal_path = root / "journal.jsonl"
            swap = FullTreeSwap(stage, WriteAheadJournal(journal_path))
            swap.execute()
            self.assertEqual((active / "hyprland.conf").read_text(), "realmheart=v2\n")
            self.assertTrue(stage.old.exists())
            swap.rollback()
            self.assertEqual((active / "hyprland.conf").read_text(), "realmheart=v1\n")
            self.assertFalse(stage.old.exists())
            self.assertTrue(stage.staging.exists())

    def test_fresh_install_swaps_staging_into_absent_target(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            release = root / "release-hypr"
            release.mkdir()
            (release / "hyprland.conf").write_text("realmheart=fresh\n")
            config = root / "config"
            config.mkdir()
            active = config / "hypr"
            stage = prepare_full_tree_stage(
                release_tree=release,
                target=active,
                transaction_id="RH-FRESH",
            )
            journal_path = root / "journal.jsonl"
            swap = FullTreeSwap(stage, WriteAheadJournal(journal_path))
            swap.execute()
            self.assertEqual((active / "hyprland.conf").read_text(), "realmheart=fresh\n")
            self.assertFalse(stage.old.exists())
            swap.rollback()
            self.assertFalse(active.exists())
            self.assertTrue(stage.staging.exists())

    def test_restart_can_reverse_fully_swapped_but_uncommitted_transaction(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            release, active = self._fixture(root)
            stage = prepare_full_tree_stage(
                release_tree=release,
                target=active,
                transaction_id="RH-POST-SWAP-CRASH",
            )
            journal_path = root / "journal.jsonl"
            FullTreeSwap(stage, WriteAheadJournal(journal_path)).execute()
            self.assertEqual((active / "hyprland.conf").read_text(), "realmheart=v2\n")
            self.assertTrue(stage.old.exists())

            # Simulate process death before transaction COMMITTED/finalization.
            # Fresh-process rollback must reverse staging->active first, then
            # old->active, restoring both trees to their pre-transaction roles.
            rollback_transaction_from_journal(journal_path, approved_roots=[root])
            self.assertEqual((active / "hyprland.conf").read_text(), "realmheart=v1\n")
            self.assertEqual((stage.staging / "hyprland.conf").read_text(), "realmheart=v2\n")
            self.assertFalse(stage.old.exists())

    def test_restart_can_reverse_completed_first_rename_crash_window(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            release, active = self._fixture(root)
            stage = prepare_full_tree_stage(release_tree=release, target=active, transaction_id="RH-CRASH")
            journal_path = root / "journal.jsonl"
            journal = WriteAheadJournal(journal_path)

            # Simulate the first half of the swap reaching COMPLETED, followed
            # by power loss before staging->active begins.
            from realmheart_installer.models import OperationSafety, Reversibility
            from realmheart_installer.transaction.operations import MovePathOperation, TransactionExecutor
            from realmheart_installer.transaction.preconditions import capture_path_precondition

            first = MovePathOperation(
                target=active,
                destination=stage.old,
                allowed_root=active.parent,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(active)),
            )
            TransactionExecutor(journal).execute(first)
            self.assertFalse(active.exists())
            self.assertTrue(stage.old.exists())

            # Fresh-process transaction rollback reconstructs and reverses even
            # a completed prior operation.
            rollback_transaction_from_journal(journal_path, approved_roots=[root])
            self.assertEqual((active / "hyprland.conf").read_text(), "realmheart=v1\n")
            self.assertFalse(stage.old.exists())


if __name__ == "__main__":
    unittest.main()
