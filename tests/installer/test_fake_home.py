from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_installer.context import InstallContext, XdgPaths


class FakeHomeTests(unittest.TestCase):
    def test_fake_home_and_non_default_xdg_are_isolated(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fake_home = root / "home"
            fake_config = root / "different-config-root"
            fake_state = root / "different-state-root"
            fake_data = root / "different-data-root"
            fake_cache = root / "different-cache-root"
            fake_runtime = root / "run"
            for path in (fake_home, fake_config, fake_state, fake_data, fake_cache, fake_runtime):
                path.mkdir(parents=True)

            (fake_config / "hypr").mkdir()
            (fake_config / "hypr/hyprland.conf").write_text("original")
            (fake_config / "kitty").mkdir()
            (fake_config / "kitty/kitty.conf").write_text("user kitty")
            (fake_config / "fish").mkdir()
            (fake_config / "fish/config.fish").write_text("user fish")

            env = {
                "HOME": str(fake_home),
                "XDG_CONFIG_HOME": str(fake_config),
                "XDG_STATE_HOME": str(fake_state),
                "XDG_DATA_HOME": str(fake_data),
                "XDG_CACHE_HOME": str(fake_cache),
                "XDG_RUNTIME_DIR": str(fake_runtime),
            }
            paths = XdgPaths.resolve(env=env, uid=1000)
            context = InstallContext.create(paths=paths, transaction_id="RH-FAKE", dry_run=True)

            self.assertTrue(context.transaction_dir.is_dir())
            self.assertEqual((fake_config / "hypr/hyprland.conf").read_text(), "original")
            self.assertEqual((fake_config / "kitty/kitty.conf").read_text(), "user kitty")
            self.assertEqual((fake_config / "fish/config.fish").read_text(), "user fish")
            self.assertFalse((fake_home / ".config").exists())


if __name__ == "__main__":
    unittest.main()
