from __future__ import annotations

import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from . import _bootstrap  # noqa: F401
from realmheart_installer.context import XdgPaths
from realmheart_installer.diagnostics import DiagnosticReportStore, build_failure_report
from realmheart_installer.errors import InstallerError


class FailureReportingTests(unittest.TestCase):
    def _paths(self, root: Path) -> XdgPaths:
        home = root / "home" / "alice"
        env = {
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "XDG_STATE_HOME": str(home / ".state"),
            "XDG_DATA_HOME": str(home / ".data"),
            "XDG_CACHE_HOME": str(home / ".cache"),
            "XDG_RUNTIME_DIR": str(root / "run" / "alice"),
        }
        for value in env.values():
            Path(value).mkdir(parents=True, exist_ok=True)
        return XdgPaths.resolve(env=env, uid=os.getuid())

    def test_structured_failure_report_is_machine_authored_and_redacted(self) -> None:
        exc = InstallerError(
            "failed under /home/alice/private password=synthetic-only",
            code="RH_FIXTURE_FAILED", stage="planning",
        )
        payload, markdown, github, title = build_failure_report(
            exc, transaction_id="RH-TEST", operation="install",
        )
        self.assertEqual(payload["code"], "RH_FIXTURE_FAILED")
        self.assertTrue(str(payload["incident_fingerprint"]).startswith("RH-INS-FPRINT-"))
        self.assertIn("[Installer] RH_FIXTURE_FAILED", title)
        for text in (json.dumps(payload), markdown, github):
            self.assertNotIn("synthetic-only", text)
            self.assertNotIn("/home/alice", text)
        self.assertIn("Generated automatically by Realmheart diagnostics", github)

    def test_unexpected_failure_origin_uses_basenames_not_absolute_paths(self) -> None:
        try:
            raise RuntimeError("fixture explosion")
        except RuntimeError as exc:
            payload, _, github, _ = build_failure_report(
                exc, transaction_id="RH-TEST", operation="install",
            )
        self.assertEqual(payload["code"], "RH_UNEXPECTED_FAILURE")
        self.assertTrue(payload["origin_frames"])
        self.assertNotIn(str(Path(__file__).resolve().parent), github)
        self.assertIn("test_failure_reporting.py", github)

    def test_failure_bundle_uses_existing_private_diagnostic_store_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            payload, markdown, github, _ = build_failure_report(
                InstallerError("fixture", code="RH_FIXTURE", stage="test"),
                transaction_id="RH-TEST", operation="install",
            )
            store = DiagnosticReportStore(paths)
            bundle = store.save_rendered(
                str(payload["incident_id"]), payload=payload,
                markdown=markdown, github=github,
            )
            self.assertEqual(bundle.github_path.stat().st_mode & 0o777, 0o600)
            loaded, persisted, persisted_markdown = store.inspect(bundle.incident_id)
            self.assertEqual(loaded.incident_id, bundle.incident_id)
            self.assertEqual(persisted["code"], "RH_FIXTURE")
            self.assertIn("Realmheart Installer terminal failure", persisted_markdown)


    def test_cli_failure_path_persists_bundle_and_offers_report(self) -> None:
        from contextlib import nullcontext
        from realmheart_installer.cli import main

        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            with patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.XdgPaths.resolve", return_value=paths), \
                 patch("realmheart_installer.cli.InstallerLock", side_effect=lambda *a, **k: nullcontext()), \
                 patch("realmheart_installer.cli.generate_transaction_id", return_value="RH-TEST-FAILURE"), \
                 patch("realmheart_installer.cli.PreflightScanner.scan", side_effect=RuntimeError("fixture explosion")), \
                 patch("realmheart_installer.cli.offer_github_issue") as offer:
                self.assertEqual(main(["preflight"]), 1)
            bundles = DiagnosticReportStore(paths).list()
            self.assertEqual(len(bundles), 1)
            _, payload, _ = DiagnosticReportStore(paths).inspect(bundles[0].incident_id)
            self.assertEqual(payload["code"], "RH_UNEXPECTED_FAILURE")
            offer.assert_called_once()

    def test_expected_cli_error_is_recorded_without_github_nudge(self) -> None:
        from argparse import Namespace
        from realmheart_installer.cli import _persist_terminal_failure_report

        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            args = Namespace(command="install", json=False)
            with patch("realmheart_installer.cli.offer_github_issue") as offer:
                _persist_terminal_failure_report(
                    args=args,
                    exc=InstallerError("bad flags", code="RH_CLI_FLAG_CONFLICT", stage="cli"),
                    paths=paths, transaction_id="RH-TEST", snapshot=None, plan=None,
                )
            self.assertEqual(len(DiagnosticReportStore(paths).list()), 1)
            offer.assert_not_called()

    def test_cli_failure_reporting_never_masks_primary_error_if_reporting_fails(self) -> None:
        from argparse import Namespace
        from realmheart_installer.cli import _persist_terminal_failure_report

        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            args = Namespace(command="install", json=False)
            with patch(
                "realmheart_installer.cli.DiagnosticReportStore.save_rendered",
                side_effect=OSError("report sink failed"),
            ):
                # The helper is deliberately best-effort and must return normally.
                _persist_terminal_failure_report(
                    args=args,
                    exc=InstallerError("primary", code="RH_PRIMARY", stage="test"),
                    paths=paths,
                    transaction_id="RH-TEST",
                    snapshot=None,
                    plan=None,
                )


if __name__ == "__main__":
    unittest.main()
