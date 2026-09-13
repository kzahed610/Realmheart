from __future__ import annotations

import json
import os
import stat
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

from . import _bootstrap  # noqa: F401
from realmheart_installer.context import XdgPaths
from realmheart_installer.diagnostics import DiagnosticReportBuilder, DiagnosticReportStore, render_github_issue, render_json_report, render_markdown_report
from realmheart_installer.diagnostics.codes import verification_error_code
from realmheart_installer.errors import InstallerError
from realmheart_installer.models import FxCompatibility, InstallMode
from realmheart_installer.environment.detect import DisplayInfo, DistroInfo, HyprlandInfo, PackageManagerInfo, SessionInfo
from realmheart_installer.environment.installation import InstallOrigin
from realmheart_installer.environment.preflight import PreflightState
from realmheart_installer.environment.support import HyprlandCompatibility, parse_version
from realmheart_installer.verification.models import ComponentHealthState, InstallHealthState, RuntimeHealthState, ActivationState, VerificationCheckState


class Phase15DiagnosticsTests(unittest.TestCase):
    def _paths(self, root: Path, username: str = "alice") -> XdgPaths:
        home = root / "home" / username
        env = {
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / ".cfg"),
            "XDG_STATE_HOME": str(home / ".state"),
            "XDG_DATA_HOME": str(home / ".data"),
            "XDG_CACHE_HOME": str(home / ".cache"),
            "XDG_RUNTIME_DIR": str(root / "run" / username),
        }
        for item in env.values():
            Path(item).mkdir(parents=True, exist_ok=True)
        return XdgPaths.resolve(env=env, uid=os.getuid())

    def _snapshot(self, *, private_signature: str = "PRIVATE-INSTANCE-SIGNATURE"):
        return SimpleNamespace(
            ready=True,
            state=PreflightState.READY,
            warnings=(),
            architecture="x86_64",
            kernel="6.test",
            distro=DistroInfo("arch", "Arch Linux", "Arch Linux", None, ()),
            package_manager=PackageManagerInfo("pacman", "/usr/bin/pacman", True),
            session=SessionInfo("wayland", "wayland-1", private_signature, True, True, True),
            hyprland=HyprlandInfo(
                "/usr/bin/hyprctl", True, "0.56.2", parse_version("0.56.2"), HyprlandCompatibility.PREFERRED,
                commit="abc123", abi_hash="abi-test", dirty=False,
            ),
            displays=(DisplayInfo("eDP-1", "PRIVATE MONITOR SERIAL 123", 1920, 1080, 60.0, 1.0, 0, 0, True, False),),
            installation=SimpleNamespace(origin=InstallOrigin.LEGACY_SCRIPT, installed_version_text="0.7.8", source=SimpleNamespace(version_text="0.7.8")),
            manifest=SimpleNamespace(digest="manifest-digest"),
            blockers=(),
        )

    def _plan(self):
        return SimpleNamespace(
            ready=True,
            state=SimpleNamespace(value="ready"),
            mode=InstallMode.REINSTALL,
            current_version="0.7.8",
            target_version="0.7.8",
            manifest_digest="manifest-digest",
            plan_digest="plan-digest",
            warnings=(),
            blockers=(),
        )

    @staticmethod
    def _check(check_id: str, component: str, summary: str, state=VerificationCheckState.FAILED):
        return SimpleNamespace(id=check_id, component_id=component, summary=summary, state=state)

    def _verification(self, home: Path):
        core_check = self._check("check.core.binary.exists", "realmheart-core", f"required artifact is missing at {home}/private/file")
        core = SimpleNamespace(component_id="realmheart-core", display_name="Realmheart Core", category="core", state=ComponentHealthState.FAILED, checks=(core_check,), blocked_by=())
        event = SimpleNamespace(component_id="event-surface", display_name="Event Surface", category="essential", state=ComponentHealthState.BLOCKED, checks=(), blocked_by=("realmheart-core",))
        terminal = SimpleNamespace(component_id="terminal", display_name="Realmheart Terminal", category="qol", state=ComponentHealthState.BLOCKED, checks=(), blocked_by=("event-surface",))
        healthy = SimpleNamespace(component_id="night-light", display_name="Night Light", category="qol", state=ComponentHealthState.HEALTHY, checks=(), blocked_by=())
        return SimpleNamespace(
            ok=False,
            install_health=InstallHealthState.FAILED,
            activation=SimpleNamespace(state=ActivationState.UNKNOWN, runtime_health=RuntimeHealthState.UNKNOWN),
            components=(core, event, terminal, healthy),
            checks=(core_check,),
            artifacts=(),
            warnings=(),
        )

    def test_report_groups_root_failure_and_transitive_blocked_components(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            report = DiagnosticReportBuilder(
                paths=paths,
                source_root=Path(temp) / "source",
                transaction_id="RH-ONE",
                snapshot=self._snapshot(),
                plan=self._plan(),
                verification=self._verification(paths.home),
                now=datetime(2026, 9, 13, 4, 0, tzinfo=timezone.utc),
            ).build()
            self.assertEqual(len(report.root_failures), 1)
            root = report.root_failures[0]
            self.assertEqual(root.component_id, "realmheart-core")
            self.assertEqual(root.error_codes, ("RH_VERIFY_CORE_BINARY_EXISTS_FAILED",))
            self.assertEqual(set(root.affected_components), {"realmheart-core", "event-surface", "terminal"})
            self.assertEqual(len(report.blocked_components), 2)

    def test_report_never_serializes_private_environment_or_monitor_description(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp), "secret-user")
            secret = "TOP-SECRET-API-TOKEN-123"
            old = os.environ.get("REALMHEART_TEST_SECRET")
            os.environ["REALMHEART_TEST_SECRET"] = secret
            try:
                snapshot = self._snapshot(private_signature="VERY-PRIVATE-HYPR-SIGNATURE")
                snapshot = SimpleNamespace(**{**snapshot.__dict__, "warnings": (f"legacy path {paths.home}/private",)})
                report = DiagnosticReportBuilder(
                    paths=paths,
                    source_root=Path(temp) / "source",
                    transaction_id="RH-PRIVACY",
                    snapshot=snapshot,
                    plan=self._plan(),
                    verification=self._verification(paths.home),
                ).build()
            finally:
                if old is None:
                    os.environ.pop("REALMHEART_TEST_SECRET", None)
                else:
                    os.environ["REALMHEART_TEST_SECRET"] = old
            rendered = render_json_report(report) + render_markdown_report(report) + render_github_issue(report)
            self.assertNotIn(secret, rendered)
            self.assertNotIn("VERY-PRIVATE-HYPR-SIGNATURE", rendered)
            self.assertNotIn("PRIVATE MONITOR SERIAL", rendered)
            self.assertNotIn(str(paths.home), rendered)
            self.assertIn("$HOME", rendered)

    def test_required_fx_bridge_is_not_reported_as_second_root_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            fx_check = self._check("verify.fx.live-artifacts", "realmheart-fx", "required FX plugin identity is incomplete")
            bridge = self._check("verify.core.required-fx", "realmheart-core", "required FX is unhealthy")
            fx = SimpleNamespace(component_id="realmheart-fx", display_name="Realmheart FX", category="fx", state=ComponentHealthState.FAILED, checks=(fx_check,), blocked_by=())
            core = SimpleNamespace(component_id="realmheart-core", display_name="Realmheart Core", category="core", state=ComponentHealthState.FAILED, checks=(bridge,), blocked_by=())
            session = SimpleNamespace(component_id="session", display_name="Session Integration", category="essential", state=ComponentHealthState.BLOCKED, checks=(), blocked_by=("realmheart-core", "realmheart-fx"))
            verification = SimpleNamespace(ok=False, install_health=InstallHealthState.FAILED, activation=SimpleNamespace(state=ActivationState.UNKNOWN, runtime_health=RuntimeHealthState.UNKNOWN), components=(fx, core, session), checks=(fx_check, bridge), artifacts=(), warnings=())
            report = DiagnosticReportBuilder(paths=paths, source_root=Path(temp)/"src", transaction_id="RH-FX", snapshot=self._snapshot(), plan=self._plan(), verification=verification).build()
            self.assertEqual(len(report.root_failures), 1)
            self.assertEqual(report.root_failures[0].component_id, "realmheart-fx")
            self.assertIn("realmheart-core", report.root_failures[0].affected_components)
            self.assertIn("session", report.root_failures[0].affected_components)

    def test_incident_fingerprint_ignores_transaction_id_and_normalized_home(self) -> None:
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            pa = self._paths(Path(a), "one")
            pb = self._paths(Path(b), "two")
            kwargs = dict(snapshot=self._snapshot(), plan=self._plan(), now=datetime(2026, 9, 13, 4, 0, tzinfo=timezone.utc))
            ra = DiagnosticReportBuilder(paths=pa, source_root=Path(a)/"src", transaction_id="RH-A", verification=self._verification(pa.home), **kwargs).build()
            rb = DiagnosticReportBuilder(paths=pb, source_root=Path(b)/"src", transaction_id="RH-B", verification=self._verification(pb.home), **kwargs).build()
            self.assertEqual(ra.incident_fingerprint, rb.incident_fingerprint)

    def test_plan_blocker_becomes_normalized_root_cause_without_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            plan = self._plan()
            plan = SimpleNamespace(**{**plan.__dict__, "ready": False, "state": SimpleNamespace(value="blocked"), "blockers": (f"cannot inspect {paths.home}/private",)})
            report = DiagnosticReportBuilder(paths=paths, source_root=Path(temp)/"src", transaction_id="RH-BLOCK", snapshot=self._snapshot(), plan=plan).build()
            self.assertEqual(report.root_failures[0].error_codes, ("RH_PLAN_BLOCKED",))
            self.assertNotIn(str(paths.home), report.root_failures[0].summary)

    def test_report_store_is_private_inspectable_and_surgically_removable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            report = DiagnosticReportBuilder(paths=paths, source_root=Path(temp)/"src", transaction_id="RH-STORE", snapshot=self._snapshot(), plan=self._plan(), verification=self._verification(paths.home), now=datetime(2026, 9, 13, 4, 1, tzinfo=timezone.utc)).build()
            store = DiagnosticReportStore(paths)
            bundle = store.save(report)
            self.assertEqual(stat.S_IMODE(bundle.json_path.stat().st_mode), 0o600)
            loaded_bundle, payload, markdown = store.inspect(report.incident_id)
            self.assertEqual(loaded_bundle.incident_id, report.incident_id)
            self.assertEqual(payload["incident_fingerprint"], report.incident_fingerprint)
            self.assertIn("Root failures", markdown)
            self.assertEqual([item.incident_id for item in store.list()], [report.incident_id])
            store.remove(report.incident_id)
            self.assertFalse(bundle.directory.exists())

    def test_report_remove_refuses_path_traversal_and_unexpected_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            store = DiagnosticReportStore(paths)
            with self.assertRaises(InstallerError):
                store.remove("../../oops")
            report = DiagnosticReportBuilder(paths=paths, source_root=Path(temp)/"src", transaction_id="RH-STORE", snapshot=self._snapshot(), plan=self._plan(), verification=self._verification(paths.home), now=datetime(2026, 9, 13, 4, 2, tzinfo=timezone.utc)).build()
            bundle = store.save(report)
            (bundle.directory / "foreign.txt").write_text("do not delete me")
            with self.assertRaises(InstallerError):
                store.remove(report.incident_id)
            self.assertTrue((bundle.directory / "foreign.txt").exists())

    def test_failed_build_unit_becomes_root_failure_when_verification_never_ran(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self._paths(Path(temp))
            plan = self._plan()
            plan = SimpleNamespace(**{**plan.__dict__, "build_units": (SimpleNamespace(id="realmheart-fx", component_ids=("realmheart-fx",)),)})
            build = SimpleNamespace(
                ok=False, state=SimpleNamespace(value="failed"), configured=True, required_targets_built=False, self_checks_passed=False, staged_install_completed=False, live_targets_unchanged=True, staged_payload_bytes=0, provenance=None, warnings=(),
                build_units=(SimpleNamespace(build_unit_id="realmheart-fx", ok=False),),
            )
            report = DiagnosticReportBuilder(paths=paths, source_root=Path(temp)/"src", transaction_id="RH-BUILD", snapshot=self._snapshot(), plan=plan, build_report=build).build()
            self.assertEqual(len(report.root_failures), 1)
            self.assertEqual(report.root_failures[0].error_codes, ("RH_BUILD_REALMHEART_FX_FAILED",))
            self.assertEqual(report.root_failures[0].affected_components, ("realmheart-fx",))

    def test_normalized_error_code_is_stable(self) -> None:
        self.assertEqual(verification_error_code("verify.security.auth-helper.mode"), "RH_VERIFY_SECURITY_AUTH_HELPER_MODE_FAILED")
        self.assertEqual(verification_error_code("check.core.binary.exists"), "RH_VERIFY_CORE_BINARY_EXISTS_FAILED")


if __name__ == "__main__":
    unittest.main()
