from __future__ import annotations

import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

from . import _bootstrap  # noqa: F401
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.configuration.terminal import KITTY_BEGIN, KITTY_END
from realmheart_installer.context import InstallContext, XdgPaths
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.filesystem.backup import create_backup_snapshot, validate_backup_snapshot
from realmheart_installer.filesystem.compare import fingerprint_path
from realmheart_installer.models import OperationState
from realmheart_installer.transaction.journal import WriteAheadJournal
from realmheart_installer.uninstall import UninstallConfigAction, UninstallExecutor, UninstallPlanner
from realmheart_installer.uninstall.models import DifferenceKind, FootprintAction


class NoSystemRunner:
    def which(self, executable: str):
        return None

    def run(self, argv, **kwargs):
        return CommandResult(tuple(str(item) for item in argv), 127, stderr="unavailable in test")


class FailingSystemdRunner:
    def which(self, executable: str):
        return "/usr/bin/systemctl" if executable == "systemctl" else None

    def run(self, argv, **kwargs):
        command = tuple(str(item) for item in argv)
        if "is-enabled" in command:
            return CommandResult(command, 0, stdout="enabled\n")
        if "is-active" in command:
            return CommandResult(command, 0, stdout="active\n")
        if "disable" in command and "--now" in command:
            return CommandResult(command, 1, stderr="synthetic quiesce failure")
        return CommandResult(command, 0)


def paths_for(root: Path) -> XdgPaths:
    env = {
        "HOME": str(root / "home"),
        "XDG_CONFIG_HOME": str(root / "cfg"),
        "XDG_STATE_HOME": str(root / "state"),
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "XDG_RUNTIME_DIR": str(root / "run"),
    }
    for value in env.values():
        Path(value).mkdir(parents=True, exist_ok=True)
    return XdgPaths.resolve(env=env, uid=os.getuid())


class Phase17UninstallTests(unittest.TestCase):
    def _fixture(self, root: Path):
        paths = paths_for(root)
        registry = load_manifest(_bootstrap.REPO_ROOT / "components")
        install_txid = "RH-20260913-120000-INST"

        hypr = paths.config_home / "hypr"
        hypr.mkdir(parents=True)
        (hypr / "hyprland.conf").write_text("pre-realmheart\n", encoding="utf-8")
        (hypr / "custom").mkdir()
        (hypr / "custom/user.conf").write_text("pre-custom\n", encoding="utf-8")
        kitty = paths.config_home / "kitty/kitty.conf"
        kitty.parent.mkdir(parents=True)
        kitty.write_text("font_size 12\n", encoding="utf-8")

        dropin = paths.config_home / "kitty/realmheart-theme.conf"
        core_service = paths.config_home / "systemd/user/realmheart.service"
        seed = paths.config_home / "realmheart/scripts/search_image.sh"

        create_backup_snapshot(
            paths.baseline_backup,
            {
                "config.hypr.takeover": hypr,
                "config.kitty.managed-block": kitty,
                "config.artifact.terminal.kitty-dropin": dropin,
                "config.generated.core.service": core_service,
                "config.realmheart.seed.scripts.search_image.sh": seed,
            },
            snapshot_kind="permanent_baseline",
            installer_version="test",
            target_realmheart_version="0.7.8",
            transaction_id="RH-BASE",
        )
        self.assertTrue(validate_backup_snapshot(paths.baseline_backup).valid)
        baseline_hypr_fp = fingerprint_path(paths.baseline_backup / "content/config.hypr.takeover")
        baseline_kitty = (paths.baseline_backup / "content/config.kitty.managed-block").read_bytes()

        shutil.rmtree(hypr)
        hypr.mkdir()
        (hypr / "hyprland.conf").write_text("realmheart-default\n", encoding="utf-8")
        (hypr / "custom").mkdir()
        (hypr / "custom/user.conf").write_text("pre-custom\n", encoding="utf-8")

        kitty.write_text(
            "font_size 12\n\n"
            f"{KITTY_BEGIN}\n"
            f"include {dropin}\n"
            f"{KITTY_END}\n",
            encoding="utf-8",
        )
        dropin.write_text("include generated-theme\n", encoding="utf-8")
        core_service.parent.mkdir(parents=True, exist_ok=True)
        core_service.write_text("[Service]\nExecStart=/usr/local/bin/realmheart\n", encoding="utf-8")
        seed.parent.mkdir(parents=True, exist_ok=True)
        seed.write_text("#!/bin/sh\necho realmheart\n", encoding="utf-8")

        theme = paths.realmheart_state / "theme"
        theme.mkdir(parents=True, exist_ok=True)
        generated = {
            "terminal.generated-kitty": theme / "kitty-theme.conf",
            "terminal.generated-fish": theme / "fish-theme.fish",
            "terminal.generated-starship": theme / "starship.toml",
            "terminal.generated-rail": theme / "rail.png",
        }
        for artifact_id, path in generated.items():
            path.write_bytes((artifact_id + "\n").encode())
        events = paths.realmheart_state / "events.db"
        events.write_bytes(b"event-history")
        palette = paths.realmheart_state / "theme-palette.tsv"
        palette.write_bytes(b"palette")

        install_dir = paths.transactions / install_txid
        install_dir.mkdir(parents=True, exist_ok=True)
        journal = WriteAheadJournal(install_dir / "journal.jsonl")
        managed_targets = [dropin, core_service, seed, *generated.values()]
        for index, target in enumerate(managed_targets, start=1):
            op = f"install-{index}"
            journal.append(
                operation_id=op,
                state=OperationState.INTENT,
                kind="write_file",
                target=str(target),
                data={"expected_after_fingerprint": fingerprint_path(target)},
            )
            journal.append(operation_id=op, state=OperationState.STARTED, kind="write_file", target=str(target))
            journal.append(operation_id=op, state=OperationState.COMPLETED, kind="write_file", target=str(target))
        # One package installed by Realmheart and one pre-existing package prove
        # that Phase 17 filters cleanup candidates by install provenance.
        (install_dir / "transaction.json").write_text(json.dumps({
            "metadata": {"package_install": {"provenance": [
                {"package": "wl-clipboard", "installed_by_transaction": True, "version_after": "1.0", "required_by": ["runtime.wl-paste"]},
                {"package": "fish", "installed_by_transaction": False, "version_after": "4.0", "required_by": ["runtime.fish"]},
            ]}}
        }), encoding="utf-8")

        receipt_artifacts = {
            "hypr.tree": self._artifact("hypr.tree", hypr, "directory", "user"),
            "terminal.kitty-dropin": self._artifact("terminal.kitty-dropin", dropin, "config", "user"),
            "core.service": self._artifact("core.service", core_service, "service", "user"),
        }
        for artifact_id, path in generated.items():
            receipt_artifacts[artifact_id] = self._artifact(artifact_id, path, "generated", "user")
        receipt = {
            "schema_version": 2,
            "realmheart_version": "0.7.8",
            "manifest_set_sha256": registry.digest,
            "transaction_id": install_txid,
            "disposition": "kept",
            "artifacts": receipt_artifacts,
        }
        paths.realmheart_state.mkdir(parents=True, exist_ok=True)
        receipt_path = paths.realmheart_state / "installed-state.json"
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

        # Years-later user edits happen after the last-managed fingerprints.
        with kitty.open("a", encoding="utf-8") as handle:
            handle.write("# user-after-install\n")
        (hypr / "custom/user.conf").write_text("years-later-custom\n", encoding="utf-8")
        (hypr / "extra.conf").write_text("years-later-extra\n", encoding="utf-8")

        return {
            "paths": paths,
            "registry": registry,
            "install_txid": install_txid,
            "hypr": hypr,
            "kitty": kitty,
            "dropin": dropin,
            "service": core_service,
            "seed": seed,
            "generated": generated,
            "events": events,
            "palette": palette,
            "baseline_hypr_fp": baseline_hypr_fp,
            "baseline_kitty": baseline_kitty,
            "receipt": receipt_path,
        }

    @staticmethod
    def _artifact(artifact_id: str, path: Path, artifact_type: str, ownership: str):
        return {
            "component_id": "test",
            "path": str(path),
            "type": artifact_type,
            "ownership": ownership,
            "mode": None,
            "uid": None,
            "gid": None,
            "size_bytes": None,
            "sha256": None,
            "immutable_fingerprint": None,
        }

    def _plan(self, fixture, txid="RH-20260913-130000-UNIN"):
        return UninstallPlanner(
            paths=fixture["paths"],
            source_root=_bootstrap.REPO_ROOT,
            transaction_id=txid,
            registry=fixture["registry"],
        ).build()

    def test_planner_detects_years_later_config_divergence_and_package_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            plan = self._plan(fixture)
            self.assertTrue(plan.ready, plan.blockers)
            self.assertTrue(plan.has_config_divergence)
            hypr_compare = next(item for item in plan.comparisons if item.target == str(fixture["hypr"]))
            kinds = {item.kind for item in hypr_compare.differences}
            self.assertIn(DifferenceKind.ADDED, kinds)
            self.assertIn(DifferenceKind.CHANGED, kinds)
            self.assertEqual([item.package for item in plan.package_cleanup_candidates], ["wl-clipboard"])
            self.assertIn(str(fixture["events"]), plan.preserved_paths)
            self.assertIn(str(fixture["palette"]), plan.preserved_paths)

    def test_keep_current_removes_realmheart_integration_but_preserves_user_config_and_history(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            plan = self._plan(fixture)
            before_hypr = fingerprint_path(fixture["hypr"])
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                config_action=UninstallConfigAction.KEEP_CURRENT,
            ).run()
            self.assertTrue(result.completed, result.errors)
            self.assertEqual(fingerprint_path(fixture["hypr"]), before_hypr)
            kitty_text = fixture["kitty"].read_text(encoding="utf-8")
            self.assertNotIn(KITTY_BEGIN, kitty_text)
            self.assertNotIn(KITTY_END, kitty_text)
            self.assertIn("# user-after-install", kitty_text)
            self.assertFalse(fixture["dropin"].exists())
            self.assertFalse(fixture["service"].exists())
            self.assertFalse(fixture["seed"].exists())
            self.assertTrue(all(not path.exists() for path in fixture["generated"].values()))
            self.assertEqual(fixture["events"].read_bytes(), b"event-history")
            self.assertEqual(fixture["palette"].read_bytes(), b"palette")
            self.assertFalse(fixture["receipt"].exists())
            self.assertIsNone(result.safety_snapshot)

    def test_restore_baseline_is_staged_and_creates_pre_uninstall_safety_snapshot(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            plan = self._plan(fixture)
            current_hypr_fp = fingerprint_path(fixture["hypr"])
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                config_action=UninstallConfigAction.RESTORE_BASELINE,
            ).run()
            self.assertTrue(result.completed, result.errors)
            self.assertEqual(fingerprint_path(fixture["hypr"]), fixture["baseline_hypr_fp"])
            self.assertEqual(fixture["kitty"].read_bytes(), fixture["baseline_kitty"])
            self.assertIsNotNone(result.safety_snapshot)
            snapshot = Path(result.safety_snapshot)
            self.assertTrue(validate_backup_snapshot(snapshot).valid)
            manifest = json.loads((snapshot / "manifest.json").read_text(encoding="utf-8"))
            hypr_record = next(item for item in manifest["sources"] if item["source"] == str(fixture["hypr"]))
            self.assertEqual(hypr_record["source_fingerprint"], current_hypr_fp)
            self.assertEqual(fixture["events"].read_bytes(), b"event-history")
            self.assertFalse(fixture["receipt"].exists())

    def test_restore_baseline_rejects_kitty_change_after_plan_and_rolls_back_hypr(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            plan = self._plan(fixture)
            before_hypr = fingerprint_path(fixture["hypr"])
            fixture["kitty"].write_text(
                fixture["kitty"].read_text(encoding="utf-8") + "# changed-after-plan\n",
                encoding="utf-8",
            )
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                config_action=UninstallConfigAction.RESTORE_BASELINE,
            ).run()
            self.assertFalse(result.completed)
            self.assertTrue(result.rolled_back, result.errors)
            self.assertEqual(fingerprint_path(fixture["hypr"]), before_hypr)
            self.assertIn("changed after uninstall planning", " ".join(result.errors))
            self.assertTrue(fixture["receipt"].exists())

    def test_running_service_quiesce_failure_blocks_file_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            plan = self._plan(fixture)
            before_service = fixture["service"].read_bytes()
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=FailingSystemdRunner(),
                config_action=UninstallConfigAction.KEEP_CURRENT,
            ).run()
            self.assertFalse(result.completed)
            self.assertTrue(result.rolled_back, result.errors)
            self.assertIn("could not quiesce Realmheart user unit", " ".join(result.errors))
            self.assertEqual(fixture["service"].read_bytes(), before_service)
            self.assertTrue(fixture["receipt"].exists())

    def test_diverged_owned_file_keep_current_fails_closed_and_rolls_back(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            fixture["dropin"].write_text("user modified this Realmheart-named file\n", encoding="utf-8")
            plan = self._plan(fixture)
            entry = next(item for item in plan.footprint if item.artifact_id == "terminal.kitty-dropin")
            self.assertTrue(entry.diverged)
            self.assertEqual(entry.keep_current_action, FootprintAction.PRESERVE_CONFLICT)
            before_kitty = fixture["kitty"].read_bytes()
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                config_action=UninstallConfigAction.KEEP_CURRENT,
            ).run()
            self.assertFalse(result.completed)
            self.assertTrue(result.rolled_back, result.errors)
            self.assertTrue(fixture["receipt"].exists())
            self.assertEqual(fixture["kitty"].read_bytes(), before_kitty)
            self.assertTrue(fixture["service"].exists())
            self.assertTrue(fixture["dropin"].exists())

    def test_explicit_event_history_purge_removes_only_events_db_not_palette(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            plan = self._plan(fixture)
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                config_action=UninstallConfigAction.KEEP_CURRENT,
                purge_event_history=True,
            ).run()
            self.assertTrue(result.completed, result.errors)
            self.assertFalse(fixture["events"].exists())
            self.assertTrue(fixture["palette"].exists())

    def test_generated_theme_drift_remains_owned_and_is_removed(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            generated = fixture["generated"]["terminal.generated-kitty"]
            generated.write_text("regenerated after wallpaper change\n", encoding="utf-8")
            plan = self._plan(fixture)
            entry = next(item for item in plan.footprint if item.artifact_id == "terminal.generated-kitty")
            self.assertTrue(entry.diverged)
            self.assertEqual(entry.keep_current_action, FootprintAction.REMOVE)
            context = InstallContext.create(paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id)
            result = UninstallExecutor(
                plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                config_action=UninstallConfigAction.KEEP_CURRENT,
            ).run()
            self.assertTrue(result.completed, result.errors)
            self.assertFalse(generated.exists())

    def test_unknown_receipt_artifact_blocks_receipt_retirement(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            payload = json.loads(fixture["receipt"].read_text(encoding="utf-8"))
            payload["artifacts"]["future.unknown-artifact"] = self._artifact(
                "future.unknown-artifact", Path(temp) / "unknown", "file", "user"
            )
            fixture["receipt"].write_text(json.dumps(payload), encoding="utf-8")
            plan = self._plan(fixture)
            self.assertFalse(plan.ready)
            self.assertTrue(any("unknown to the current canonical manifest" in item for item in plan.blockers))

    def test_receipt_type_or_ownership_tampering_cannot_change_uninstall_behavior(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            payload = json.loads(fixture["receipt"].read_text(encoding="utf-8"))
            payload["artifacts"]["core.service"]["type"] = "config"
            fixture["receipt"].write_text(json.dumps(payload), encoding="utf-8")
            plan = self._plan(fixture)
            self.assertFalse(plan.ready)
            self.assertTrue(any("type conflicts with the canonical manifest" in item for item in plan.blockers))

    def test_receipt_path_tampering_cannot_authorize_arbitrary_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = self._fixture(Path(temp))
            payload = json.loads(fixture["receipt"].read_text(encoding="utf-8"))
            payload["artifacts"]["terminal.kitty-dropin"]["path"] = str(Path(temp) / "victim")
            fixture["receipt"].write_text(json.dumps(payload), encoding="utf-8")
            plan = self._plan(fixture)
            self.assertFalse(plan.ready)
            self.assertTrue(any("not authorized by the canonical manifest" in item for item in plan.blockers))

    def test_new_baseline_records_privileged_restore_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source"
            source.write_text("x", encoding="utf-8")
            source.chmod(0o640)
            snapshot = root / "snapshot"
            create_backup_snapshot(
                snapshot, {"one": source}, snapshot_kind="test", installer_version="test",
                target_realmheart_version="test", transaction_id="RH-META",
            )
            manifest = json.loads((snapshot / "manifest.json").read_text(encoding="utf-8"))
            record = manifest["sources"][0]
            self.assertEqual(record["source_type"], "file")
            self.assertEqual(record["source_mode"], "0640")
            self.assertEqual(record["source_uid"], source.stat().st_uid)
            self.assertEqual(record["source_gid"], source.stat().st_gid)


if __name__ == "__main__":
    unittest.main()
