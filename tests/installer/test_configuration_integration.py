from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from . import _bootstrap
from realmheart_installer.configuration import ConfigurationIntegrator
from realmheart_installer.context import XdgPaths
from realmheart_installer.environment.command import CommandResult, CommandRunner
from realmheart_installer.filesystem.compare import fingerprint_path
from realmheart_installer.models import Reversibility
from realmheart_installer.planning.models import ArtifactCommitClass, ConfigAction, ConfigActionKind
from realmheart_installer.transaction.journal import WriteAheadJournal, read_journal


class HybridRunner:
    """Run Python for real; emulate Fish/Starship/systemd user state."""

    def __init__(self) -> None:
        self.enabled = False
        self.active = False
        self.commands: list[tuple[str, ...]] = []
        self.real = CommandRunner()

    def which(self, executable: str):
        if executable in {"python3", "python"}:
            return sys.executable
        if executable == "fish":
            return "/fake/fish"
        if executable == "starship":
            return "/fake/starship"
        if executable == "systemctl":
            return "/fake/systemctl"
        return None

    def run(self, argv, **kwargs):
        command = tuple(str(x) for x in argv)
        self.commands.append(command)
        if command[0] == sys.executable:
            return self.real.run(command, **kwargs)
        if command[0] == "/fake/fish":
            return CommandResult(command, 0)
        if command[0] == "/fake/starship":
            return CommandResult(command, 0, "prompt")
        if command[:3] == ("/fake/systemctl", "--user", "is-enabled"):
            return CommandResult(command, 0 if self.enabled else 1, "enabled\n" if self.enabled else "disabled\n")
        if command[:3] == ("/fake/systemctl", "--user", "is-active"):
            return CommandResult(command, 0 if self.active else 3, "active\n" if self.active else "inactive\n")
        if command[:3] == ("/fake/systemctl", "--user", "daemon-reload"):
            return CommandResult(command, 0)
        if command[:4] == ("/fake/systemctl", "--user", "enable", "--now"):
            self.enabled = True
            self.active = True
            return CommandResult(command, 0)
        if command[:3] == ("/fake/systemctl", "--user", "restart"):
            return CommandResult(command, 0)
        if command[:3] == ("/fake/systemctl", "--user", "disable"):
            self.enabled = False
            return CommandResult(command, 0)
        if command[:3] == ("/fake/systemctl", "--user", "stop"):
            self.active = False
            return CommandResult(command, 0)
        if command[:3] == ("/fake/systemctl", "--user", "enable"):
            self.enabled = True
            return CommandResult(command, 0)
        if command[:3] == ("/fake/systemctl", "--user", "start"):
            self.active = True
            return CommandResult(command, 0)
        return CommandResult(command, 1, stderr="not mocked")


class FailingWatcherRunner(HybridRunner):
    def run(self, argv, **kwargs):
        command = tuple(str(x) for x in argv)
        if command[:3] == ("/fake/systemctl", "--user", "restart"):
            self.commands.append(command)
            return CommandResult(command, 1, stderr="synthetic watcher failure")
        return super().run(argv, **kwargs)


class ConfigurationIntegrationTests(unittest.TestCase):
    def _paths(self, root: Path) -> XdgPaths:
        home = root / "home"
        cfg = root / "custom-config"
        state = root / "custom-state"
        data = root / "data"
        cache = root / "cache"
        runtime = root / "run"
        for path in (home, cfg, state, data, cache, runtime):
            path.mkdir(parents=True, exist_ok=True)
        return XdgPaths.resolve(env={
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(cfg),
            "XDG_STATE_HOME": str(state),
            "XDG_DATA_HOME": str(data),
            "XDG_CACHE_HOME": str(cache),
            "XDG_RUNTIME_DIR": str(runtime),
        }, uid=1000)

    def _fixture(self, root: Path):
        paths = self._paths(root)
        cfg = paths.config_home
        state = paths.state_home

        # Existing user config that must survive according to ownership class.
        hypr = cfg / "hypr"
        (hypr / "custom").mkdir(parents=True)
        (hypr / "legacy.conf").write_text("old release tree\n", encoding="utf-8")
        (hypr / "custom/env.lua").write_text("-- user env wins\n", encoding="utf-8")
        (hypr / "custom/user-only.lua").write_text("return 'mine'\n", encoding="utf-8")

        kitty = cfg / "kitty/kitty.conf"
        kitty.parent.mkdir(parents=True)
        kitty_original = b"font_size 13\nmap ctrl+shift+x noop\n"
        kitty.write_bytes(kitty_original)

        fish_config = cfg / "fish/config.fish"
        fish_config.parent.mkdir(parents=True)
        fish_original = b"alias ll='ls -la'\nset -gx EDITOR nvim\n"
        fish_config.write_bytes(fish_original)

        preexisting_fish_theme = cfg / "fish/conf.d/realmheart-theme.fish"
        preexisting_fish_theme.parent.mkdir(parents=True, exist_ok=True)
        preexisting_fish_theme.write_text("# pre-Realmheart same-name file\n", encoding="utf-8")

        palette = state / "realmheart/theme-palette.tsv"
        palette.parent.mkdir(parents=True)
        palette_bytes = b"surface\t#101418\nprimary\t#9ccbfb\ntext\t#e2e2e6\n"
        palette.write_bytes(palette_bytes)
        unrelated = state / "realmheart/theme/unrelated.txt"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_text("do not touch\n", encoding="utf-8")

        return paths, kitty_original, fish_original, palette_bytes

    def _plan(self, paths: XdgPaths, txid="RH-P11-TEST"):
        repo = _bootstrap.REPO_ROOT
        cfg = paths.config_home
        state = paths.state_home
        actions = [
            ConfigAction(
                "config.hypr.takeover", "hypr-integration", ConfigActionKind.FULL_TREE_REPLACE,
                str(cfg / "hypr"), str(repo / "config/hypr"), True, "test", "transaction_preimage",
                Reversibility.EXACT, fingerprint_path(cfg / "hypr"), preserve=(str(cfg / "hypr/custom"),),
            ),
            ConfigAction(
                "config.kitty.managed-block", "terminal", ConfigActionKind.MANAGED_BLOCK,
                str(cfg / "kitty/kitty.conf"), None, True, "test", "transaction_preimage",
                Reversibility.GUARDED, fingerprint_path(cfg / "kitty/kitty.conf"),
                render_strategy="kitty-managed-include-v1",
                render_values=(("INCLUDE_PATH", str(cfg / "kitty/realmheart-theme.conf")),),
            ),
            ConfigAction(
                "config.fish.personal", "terminal", ConfigActionKind.READ_ONLY,
                str(cfg / "fish/config.fish"), None, False, "test", "none", Reversibility.NONE,
                fingerprint_path(cfg / "fish/config.fish"),
            ),
        ]
        source_map = {
            "terminal.kitty-dropin": "config/kitty/realmheart-terminal-public/files/.config/kitty/realmheart-theme.conf",
            "terminal.fish-theme": "config/kitty/realmheart-terminal-public/files/.config/fish/conf.d/realmheart-theme.fish",
            "terminal.fish-starship": "config/kitty/realmheart-terminal-public/files/.config/fish/conf.d/realmheart-starship.fish",
            "terminal.generator": "config/kitty/realmheart-terminal-public/files/.config/realmheart/scripts/terminal/generate-theme.py",
            "terminal.theme-service": "config/kitty/realmheart-terminal-public/files/.config/systemd/user/realmheart-terminal-theme.service",
            "terminal.theme-path": "config/kitty/realmheart-terminal-public/files/.config/systemd/user/realmheart-terminal-theme.path",
        }
        target_map = {
            "terminal.kitty-dropin": cfg / "kitty/realmheart-theme.conf",
            "terminal.fish-theme": cfg / "fish/conf.d/realmheart-theme.fish",
            "terminal.fish-starship": cfg / "fish/conf.d/realmheart-starship.fish",
            "terminal.generator": cfg / "realmheart/scripts/terminal/generate-theme.py",
            "terminal.theme-service": cfg / "systemd/user/realmheart-terminal-theme.service",
            "terminal.theme-path": cfg / "systemd/user/realmheart-terminal-theme.path",
        }
        for aid, source_rel in source_map.items():
            target = target_map[aid]
            rendered = aid in {"terminal.kitty-dropin", "terminal.theme-service", "terminal.theme-path"}
            strategy = None
            values = ()
            if aid == "terminal.kitty-dropin":
                strategy = "terminal-kitty-dropin-v1"
                values = (("STATE_THEME", str(state / "realmheart/theme/kitty-theme.conf")),)
            elif aid in {"terminal.theme-service", "terminal.theme-path"}:
                strategy = "rewrite-default-xdg-v1"
                values = (("%h/.config", str(cfg)), ("%h/.local/state", str(state)))
            actions.append(ConfigAction(
                f"config.artifact.{aid}", "terminal",
                ConfigActionKind.RENDERED_FILE if rendered else ConfigActionKind.OWNED_FILE,
                str(target), str(repo / source_rel), True, "test", "transaction_preimage",
                Reversibility.EXACT, fingerprint_path(target), render_strategy=strategy, render_values=values,
                mode="0755" if aid == "terminal.generator" else "0644",
            ))
        actions.append(ConfigAction(
            "config.terminal.generated-state", "terminal", ConfigActionKind.GENERATED_STATE,
            str(state / "realmheart/theme"), None, True, "test", "generated_state_regenerable",
            Reversibility.BEST_EFFORT, fingerprint_path(state / "realmheart/theme"),
        ))

        generated = (
            ("terminal.generated-kitty", state / "realmheart/theme/kitty-theme.conf"),
            ("terminal.generated-fish", state / "realmheart/theme/fish-theme.fish"),
            ("terminal.generated-starship", state / "realmheart/theme/starship.toml"),
            ("terminal.generated-rail", state / "realmheart/theme/rail.png"),
        )
        artifact_actions = tuple(SimpleNamespace(
            artifact_id=aid,
            component_id="terminal",
            commit_class=ArtifactCommitClass.GENERATED,
            target=str(target),
        ) for aid, target in generated)
        return SimpleNamespace(ready=True, transaction_id=txid, config_actions=tuple(actions), artifact_actions=artifact_actions)

    def _integrator(self, root: Path, paths: XdgPaths, plan, runner: HybridRunner):
        tx = root / "tx"
        tx.mkdir(exist_ok=True)
        return ConfigurationIntegrator(
            plan=plan,
            source_root=_bootstrap.REPO_ROOT,
            paths=paths,
            journal=WriteAheadJournal(tx / "journal.jsonl"),
            preimage_dir=tx / "preimages",
            runner=runner,
        ), tx

    def test_custom_xdg_full_integration_preserves_user_state_and_generates_theme(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, kitty_original, fish_original, palette_bytes = self._fixture(root)
            plan = self._plan(paths)
            runner = HybridRunner()
            integrator, tx = self._integrator(root, paths, plan, runner)

            report = integrator.apply(activate_watcher=True)
            self.assertTrue(report.ok, report.blockers)

            hypr = paths.config_home / "hypr"
            self.assertFalse((hypr / "legacy.conf").exists())
            self.assertEqual((hypr / "custom/env.lua").read_text(), "-- user env wins\n")
            self.assertEqual((hypr / "custom/user-only.lua").read_text(), "return 'mine'\n")
            self.assertTrue((hypr / "hyprland.lua").exists())

            kitty = paths.config_home / "kitty/kitty.conf"
            kitty_text = kitty.read_text()
            self.assertTrue(kitty_text.startswith(kitty_original.decode()))
            self.assertEqual(kitty_text.count("# BEGIN Realmheart Terminal Theme (managed)"), 1)
            self.assertEqual(kitty_text.count("# END Realmheart Terminal Theme (managed)"), 1)
            self.assertIn(f"include {paths.config_home / 'kitty/realmheart-theme.conf'}", kitty_text)
            self.assertEqual((paths.config_home / "fish/config.fish").read_bytes(), fish_original)
            self.assertEqual((paths.config_home / "realmheart/scripts/terminal/generate-theme.py").stat().st_mode & 0o7777, 0o755)

            dropin = (paths.config_home / "kitty/realmheart-theme.conf").read_text()
            self.assertIn(str(paths.state_home / "realmheart/theme/kitty-theme.conf"), dropin)
            service = (paths.config_home / "systemd/user/realmheart-terminal-theme.service").read_text()
            path_unit = (paths.config_home / "systemd/user/realmheart-terminal-theme.path").read_text()
            self.assertIn(str(paths.config_home), service)
            self.assertIn(str(paths.state_home), path_unit)
            self.assertNotIn("%h/.config", service)
            self.assertNotIn("%h/.local/state", path_unit)

            generated = {item.artifact_id: Path(item.path) for item in report.generated_artifacts}
            self.assertEqual(set(generated), {
                "terminal.generated-kitty", "terminal.generated-fish",
                "terminal.generated-starship", "terminal.generated-rail",
            })
            for path in generated.values():
                self.assertTrue(path.is_file())
                self.assertGreater(path.stat().st_size, 0)
            self.assertEqual((paths.state_home / "realmheart/theme-palette.tsv").read_bytes(), palette_bytes)
            self.assertEqual((paths.state_home / "realmheart/theme/unrelated.txt").read_text(), "do not touch\n")
            self.assertTrue(runner.enabled)
            self.assertTrue(runner.active)
            self.assertIn(("/fake/systemctl", "--user", "enable", "--now", "realmheart-terminal-theme.path"), runner.commands)
            self.assertIn(("/fake/systemctl", "--user", "restart", "realmheart-terminal-theme.service"), runner.commands)

            events = read_journal(tx / "journal.jsonl")
            kinds = {event.kind for event in events}
            self.assertIn("generate_terminal_theme", kinds)
            self.assertIn("systemd_user_watcher", kinds)
            self.assertIn("write_file", kinds)
            self.assertIn("move_path", kinds)

    def test_rollback_is_surgical_and_restores_preexisting_same_name_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, kitty_original, fish_original, palette_bytes = self._fixture(root)
            old_hypr = fingerprint_path(paths.config_home / "hypr")
            old_owned = (paths.config_home / "fish/conf.d/realmheart-theme.fish").read_bytes()
            plan = self._plan(paths)
            runner = HybridRunner()
            integrator, _ = self._integrator(root, paths, plan, runner)

            report = integrator.apply(activate_watcher=True)
            self.assertTrue(report.ok, report.blockers)
            integrator.rollback()

            self.assertEqual(fingerprint_path(paths.config_home / "hypr"), old_hypr)
            self.assertEqual((paths.config_home / "kitty/kitty.conf").read_bytes(), kitty_original)
            self.assertEqual((paths.config_home / "fish/config.fish").read_bytes(), fish_original)
            self.assertEqual((paths.config_home / "fish/conf.d/realmheart-theme.fish").read_bytes(), old_owned)
            self.assertFalse((paths.config_home / "fish/conf.d/realmheart-starship.fish").exists())
            self.assertFalse((paths.config_home / "kitty/realmheart-theme.conf").exists())
            self.assertFalse((paths.config_home / "realmheart/scripts/terminal/generate-theme.py").exists())
            self.assertEqual((paths.state_home / "realmheart/theme-palette.tsv").read_bytes(), palette_bytes)
            self.assertEqual((paths.state_home / "realmheart/theme/unrelated.txt").read_text(), "do not touch\n")
            for name in ("kitty-theme.conf", "fish-theme.fish", "starship.toml", "rail.png"):
                self.assertFalse((paths.state_home / "realmheart/theme" / name).exists())
            self.assertFalse(runner.enabled)
            self.assertFalse(runner.active)

    def test_reinstall_is_idempotent_for_kitty_and_fish_personal_config(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, _, fish_original, _ = self._fixture(root)
            first_runner = HybridRunner()
            first, _ = self._integrator(root, paths, self._plan(paths, "RH-P11-A"), first_runner)
            self.assertTrue(first.apply(activate_watcher=False).ok)

            second_runner = HybridRunner()
            second, _ = self._integrator(root, paths, self._plan(paths, "RH-P11-B"), second_runner)
            second_report = second.apply(activate_watcher=False)
            self.assertTrue(second_report.ok, second_report.blockers)
            kitty_text = (paths.config_home / "kitty/kitty.conf").read_text()
            self.assertEqual(kitty_text.count("# BEGIN Realmheart Terminal Theme (managed)"), 1)
            self.assertEqual(kitty_text.count("# END Realmheart Terminal Theme (managed)"), 1)
            self.assertEqual((paths.config_home / "fish/config.fish").read_bytes(), fish_original)

    def test_watcher_failure_rolls_back_configuration_and_previous_unit_state(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, kitty_original, fish_original, _ = self._fixture(root)
            old_hypr = fingerprint_path(paths.config_home / "hypr")
            runner = FailingWatcherRunner()
            integrator, _ = self._integrator(root, paths, self._plan(paths), runner)
            report = integrator.apply(activate_watcher=True, rollback_on_failure=True)
            self.assertFalse(report.ok)
            self.assertTrue(report.rolled_back)
            self.assertTrue(any("RH_TERMINAL_WATCHER_FAILED" in item for item in report.blockers))
            self.assertEqual(fingerprint_path(paths.config_home / "hypr"), old_hypr)
            self.assertEqual((paths.config_home / "kitty/kitty.conf").read_bytes(), kitty_original)
            self.assertEqual((paths.config_home / "fish/config.fish").read_bytes(), fish_original)
            self.assertFalse(runner.enabled)
            self.assertFalse(runner.active)

    def test_plan_drift_blocks_before_any_configuration_mutation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths, kitty_original, _, _ = self._fixture(root)
            plan = self._plan(paths)
            (paths.config_home / "kitty/kitty.conf").write_bytes(kitty_original + b"# concurrent edit\n")
            runner = HybridRunner()
            integrator, tx = self._integrator(root, paths, plan, runner)
            report = integrator.apply(activate_watcher=False)
            self.assertFalse(report.ok)
            self.assertTrue(any("RH_PRECONDITION_DRIFT" in item for item in report.blockers))
            self.assertTrue((paths.config_home / "hypr/legacy.conf").exists())
            self.assertFalse((paths.config_home / "fish/conf.d/realmheart-starship.fish").exists())
            self.assertEqual(read_journal(tx / "journal.jsonl"), [])


if __name__ == "__main__":
    unittest.main()
