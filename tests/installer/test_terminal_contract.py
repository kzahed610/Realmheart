from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.models import OperationSafety, Reversibility
from realmheart_installer.transaction.journal import WriteAheadJournal
from realmheart_installer.transaction.operations import TransactionExecutor, WriteFileOperation
from realmheart_installer.transaction.preconditions import capture_path_precondition


REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "config/kitty/realmheart-terminal-public/files/.config/realmheart/scripts/terminal/generate-theme.py"


class TerminalContractTests(unittest.TestCase):
    def test_fish_dropins_do_not_touch_config_fish(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fish = root / "config/fish"
            conf_d = fish / "conf.d"
            conf_d.mkdir(parents=True)
            personal = fish / "config.fish"
            personal.write_text("set -gx MY_PERSONAL_VALUE yes\n")
            original = personal.read_bytes()

            target = conf_d / "realmheart-theme.fish"
            op = WriteFileOperation(
                target=target,
                allowed_root=root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(root / "preimage")),
                content=b"# realmheart drop-in\n",
                mode=0o644,
                preimage_path=root / "preimage",
            )
            TransactionExecutor(WriteAheadJournal(root / "journal.jsonl")).execute(op)
            self.assertEqual(personal.read_bytes(), original)
            self.assertEqual(target.read_text(), "# realmheart drop-in\n")

    def test_terminal_generator_writes_only_machine_local_generated_state(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            config = root / "cfg"
            state = root / "state"
            home.mkdir()
            config.mkdir()
            state.mkdir()

            # Give it a complete palette so the test also proves custom XDG
            # state discovery instead of relying only on fallback colors.
            realmheart_state = state / "realmheart"
            realmheart_state.mkdir()
            (realmheart_state / "theme-palette.tsv").write_text(
                "surface\t#101418\nprimary\t#9ccbfb\ntext\t#e2e2e6\nerror\t#ffb4ab\n"
            )

            env = os.environ.copy()
            env.update(
                {
                    "HOME": str(home),
                    "XDG_CONFIG_HOME": str(config),
                    "XDG_STATE_HOME": str(state),
                }
            )
            result = subprocess.run(
                [sys.executable, str(GENERATOR)],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = realmheart_state / "theme"
            self.assertEqual(
                {path.name for path in generated.iterdir()},
                {"kitty-theme.conf", "fish-theme.fish", "starship.toml", "rail.png"},
            )
            self.assertFalse((config / "fish/config.fish").exists())
            self.assertFalse((config / "kitty/kitty.conf").exists())


if __name__ == "__main__":
    unittest.main()
