from __future__ import annotations

import json
import stat
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.context import InstallContext, XdgPaths, ensure_not_root, generate_transaction_id
from realmheart_installer.errors import InstallerError, RootExecutionError


class ContextTests(unittest.TestCase):
    def test_non_default_xdg_roots_are_honored(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = {
                "HOME": str(root / "home"),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(root / "run"),
            }
            paths = XdgPaths.resolve(env=env, uid=1000)
            self.assertEqual(paths.config_home, root / "cfg")
            self.assertEqual(paths.transactions, root / "state/realmheart-installer/transactions")
            self.assertEqual(paths.baseline_backup, root / "data/realmheart-installer/backups/baseline")
            self.assertEqual(paths.lock_path, root / "run/realmheart-installer.lock")

    def test_missing_runtime_dir_falls_back_to_private_installer_state(self) -> None:
        env = {"HOME": "/tmp/example-home", "XDG_STATE_HOME": "/tmp/example-state"}
        paths = XdgPaths.resolve(env=env, uid=1234)
        self.assertEqual(
            paths.runtime_dir,
            Path("/tmp/example-state/realmheart-installer/runtime"),
        )
        self.assertEqual(
            paths.lock_path,
            Path("/tmp/example-state/realmheart-installer/runtime/realmheart-installer.lock"),
        )

    def test_relative_xdg_value_falls_back(self) -> None:
        env = {"HOME": "/tmp/example-home", "XDG_STATE_HOME": "relative/state"}
        paths = XdgPaths.resolve(env=env, uid=1234)
        self.assertEqual(paths.state_home, Path("/tmp/example-home/.local/state"))

    def test_root_refusal(self) -> None:
        with self.assertRaises(RootExecutionError):
            ensure_not_root(euid=0)
        ensure_not_root(euid=1000)

    def test_transaction_id_shape(self) -> None:
        from datetime import datetime

        txid = generate_transaction_id(now=datetime(2026, 9, 11, 18, 4, 5), random_hex="a7f2")
        self.assertEqual(txid, "RH-20260911-180405-A7F2")

    def test_context_creates_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = {
                "HOME": str(root / "home"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(root / "run"),
            }
            paths = XdgPaths.resolve(env=env, uid=1000)
            context = InstallContext.create(paths=paths, transaction_id="RH-TEST", dry_run=True)
            summary = json.loads((context.transaction_dir / "transaction.json").read_text())
            self.assertEqual(summary["transaction_id"], "RH-TEST")
            self.assertEqual(summary["state"], "created")

    def test_private_state_namespace_roots_are_mode_0700(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = XdgPaths.resolve(env={
                "HOME": str(root / "home"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(root / "run"),
            }, uid=1000)
            InstallContext.create(paths=paths, transaction_id="RH-PRIVATE")
            for path in (paths.installer_state, paths.installer_data, paths.installer_cache, paths.realmheart_state):
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700, path)

    def test_symlinked_private_installer_state_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            state.mkdir()
            target = root / "elsewhere"
            target.mkdir()
            (state / "realmheart-installer").symlink_to(target, target_is_directory=True)
            paths = XdgPaths.resolve(env={
                "HOME": str(root / "home"),
                "XDG_STATE_HOME": str(state),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(root / "run"),
            }, uid=1000)
            with self.assertRaises(InstallerError) as captured:
                InstallContext.create(paths=paths, transaction_id="RH-SYMLINK")
            self.assertEqual(captured.exception.code, "RH_PRIVATE_STATE_UNSAFE")



if __name__ == "__main__":
    unittest.main()
