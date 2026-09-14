from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap
from realmheart_maintenance.manifest import load_manifest
from realmheart_installer.context import InstallContext
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.finalization import FinalAction
from realmheart_installer.filesystem.compare import fingerprint_path
from realmheart_installer.live import LiveInstallExecutor
from realmheart_installer.native_build.models import BuildProvenance, BuildStageReport, BuildStageState, StagedArtifactResult
from realmheart_installer.planning.planner import InstallationPlanner


class FakeLiveRunner:
    def __init__(self, plan, *, fail_event_ping: bool = False):
        self.plan = plan
        self.fail_event_ping = fail_event_ping
        self.enabled: set[str] = set()
        self.active: set[str] = set()
        self.calls: list[tuple[str, ...]] = []

    def which(self, executable):
        if executable == "python": return sys.executable
        if executable == "python3": return sys.executable
        return "/usr/bin/" + executable

    def run(self, argv, **kwargs):
        command = tuple(str(item) for item in argv)
        self.calls.append(command)
        base = Path(command[0]).name if command else ""

        if base == "systemctl" and len(command) >= 3 and command[1] == "--user":
            op = command[2]
            if op == "daemon-reload":
                return CommandResult(command, 0, "")
            if op == "is-enabled":
                svc = command[3]
                return CommandResult(command, 0 if svc in self.enabled else 1, "enabled\n" if svc in self.enabled else "disabled\n")
            if op == "is-active":
                svc = command[3]
                return CommandResult(command, 0 if svc in self.active else 3, "active\n" if svc in self.active else "inactive\n")
            if op == "enable":
                now = "--now" in command
                svc = command[-1]
                self.enabled.add(svc)
                if now: self.active.add(svc)
                return CommandResult(command, 0, "")
            if op == "disable":
                self.enabled.discard(command[-1]); return CommandResult(command, 0, "")
            if op == "start":
                self.active.add(command[-1]); return CommandResult(command, 0, "")
            if op == "stop":
                self.active.discard(command[-1]); return CommandResult(command, 0, "")
            if op == "restart":
                self.active.add(command[-1]); return CommandResult(command, 0, "")

        if len(command) >= 2 and command[1].endswith("generate-theme.py") and base.startswith("python"):
            env = dict(os.environ)
            env.update(kwargs.get("env") or {})
            proc = subprocess.run([sys.executable, command[1]], capture_output=True, text=True, env=env, timeout=20)
            return CommandResult(command, proc.returncode, proc.stdout, proc.stderr)
        if "py_compile" in command:
            return CommandResult(command, 0, "ok\n")
        if base == "fish" and "-n" in command:
            return CommandResult(command, 0, "ok\n")
        if base == "starship" and len(command) >= 2 and command[1] == "prompt":
            return CommandResult(command, 0, "prompt\n")

        if len(command) == 2 and command[1] == "--version" and command[0].endswith("/realmheart"):
            return CommandResult(command, 0, "Realmheart 0.7.8\n")
        if len(command) == 2 and command[1] == "--help" and command[0].endswith("/realmheart-event"):
            return CommandResult(command, 0, "help\n")
        if len(command) == 2 and command[1] == "ping" and command[0].endswith("/realmheart-event"):
            if self.fail_event_ping:
                return CommandResult(command, 1, "", "protocol failure")
            return CommandResult(command, 0, '{"ok":true}\n')

        if base == "hyprctl" and command[1:] == ("version", "-j"):
            return CommandResult(command, 0, json.dumps({"commit": self.plan.fx_plan.hyprland_commit, "abiHash": self.plan.fx_plan.hyprland_abi_hash, "dirty": False}) + "\n")
        if base == "hyprctl" and command[1:] == ("plugin", "list"):
            return CommandResult(command, 0, "Realmheart FX\n")
        if base == "hyprctl" and command[1:] == ("realmheart-fx", "identity"):
            return CommandResult(command, 0, (
                f"build_id={self.plan.fx_plan.build_id}\n"
                f"realmheart_version={self.plan.target_version}\n"
                f"hyprland_commit={self.plan.fx_plan.hyprland_commit}\n"
                f"hyprland_abi={self.plan.fx_plan.hyprland_abi_hash}\n"
            ))
        return CommandResult(command, 0, "ok\n")


def _sha(path: Path) -> str:
    h=hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda:handle.read(65536),b""): h.update(chunk)
    return h.hexdigest()


def staged_report(plan, root: Path) -> BuildStageReport:
    stage=root/"synthetic-stage"; stage.mkdir(parents=True,exist_ok=True)
    results=[]; total=0
    for action in plan.artifact_actions:
        if not action.required or action.commit_class.value not in {"privileged_commit","staged_payload"}:
            continue
        dest=stage/action.artifact_id.replace(".","_")
        if action.artifact_type=="directory":
            dest.mkdir(parents=True); (dest/"payload.txt").write_text(action.artifact_id+"\n"); size=(dest/"payload.txt").stat().st_size; sha=None
        else:
            dest.parent.mkdir(parents=True,exist_ok=True)
            if action.artifact_id=="auth.pam":
                dest.write_bytes((_bootstrap.REPO_ROOT/"config/pam/realmheart-lockscreen").read_bytes())
            else:
                dest.write_bytes((action.artifact_id+"\n").encode())
            mode=0o4755 if action.artifact_id=="auth.helper" else (0o755 if action.artifact_type in {"executable","library"} else 0o644)
            dest.chmod(mode); size=dest.stat().st_size; sha=_sha(dest)
        total+=size
        mode_text=f"{dest.stat().st_mode & 0o7777:04o}"
        results.append(StagedArtifactResult(
            artifact_id=action.artifact_id,
            target_path=action.target,
            staged_path=str(dest),
            artifact_type=action.artifact_type,
            required=True,
            exists=True,
            type_ok=True,
            executable_ok=True if action.artifact_type == "executable" else None,
            mode=mode_text,
            size_bytes=size,
            sha256=sha,
            fingerprint=fingerprint_path(dest),
            reason=None,
        ))
    provenance=BuildProvenance(
        plan.target_version,plan.source_revision,plan.source_dirty,plan.manifest_digest,plan.plan_digest,
        "cmake test","ninja test","c++","test","Ninja","Release",plan.layout.prefix,plan.layout.sysconf,"OFF",
        plan.fx_plan.hyprland_version,plan.fx_plan.hyprland_commit,plan.fx_plan.hyprland_abi_hash,plan.fx_plan.build_id,
    )
    return BuildStageReport(1,plan.transaction_id,BuildStageState.PASS,str(root/"build"),str(stage),True,True,True,True,True,(),True,True,(),tuple(results),(),provenance,total,(),(),(),())


class Phase16LiveInstallTests(unittest.TestCase):
    def _fixture(self, root: Path, *, fail_event_ping=False):
        from tests.installer.test_verification_engine import Phase13VerificationTests
        helper=Phase13VerificationTests(); helper.setUp()
        paths=helper._paths(root)
        # Real user-neighbor state must survive keep and rollback semantics.
        (paths.config_home/"hypr/custom").mkdir(parents=True,exist_ok=True)
        (paths.config_home/"hypr/custom/keybinds.lua").write_text("-- MY CUSTOM KEYBINDS\n")
        (paths.config_home/"hypr/old-only.txt").write_text("old hypr\n")
        (paths.config_home/"kitty").mkdir(parents=True,exist_ok=True)
        (paths.config_home/"kitty/kitty.conf").write_text("font_size 13\n")
        (paths.config_home/"fish").mkdir(parents=True,exist_ok=True)
        (paths.config_home/"fish/config.fish").write_text("set -gx PERSONAL yes\n")
        snapshot=helper._snapshot(paths)
        registry=load_manifest(_bootstrap.REPO_ROOT/"components")
        bootstrap_runner=type("PlanRunner",(),{"which":lambda self,x:"/usr/bin/"+x,"run":lambda self,argv,**kw: CommandResult(tuple(map(str,argv)),0,"ok\n")})()
        plan=InstallationPlanner(paths=paths,source_root=_bootstrap.REPO_ROOT,snapshot=snapshot,registry=registry,runner=bootstrap_runner,transaction_id="RH-LIVE-END2END",prefix=root/"prefix",sysconf=root/"etc").build()
        self.assertTrue(plan.ready,plan.blockers)
        report=staged_report(plan,root)
        runner=FakeLiveRunner(plan,fail_event_ping=fail_event_ping)
        context=InstallContext.create(paths=paths,source_root=_bootstrap.REPO_ROOT,transaction_id=plan.transaction_id)
        return paths,snapshot,registry,plan,report,runner,context

    def test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths,snapshot,registry,plan,build,runner,context=self._fixture(root)
            with patch("realmheart_installer.live.orchestrator.NativeBuildExecutor") as build_cls:
                build_cls.return_value.run.return_value=build
                result=LiveInstallExecutor(
                    plan=plan,registry=registry,context=context,paths=paths,source_root=_bootstrap.REPO_ROOT,runner=runner,snapshot=snapshot,
                    decision_selector=lambda decision: FinalAction.KEEP,package_actions_applied=True,allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(),privileged_gid=os.getgid(),persist_diagnostic_on_failure=False,
                ).run()
            self.assertEqual(result.finalization.exit_code,0)
            self.assertEqual(result.finalization.disposition,"kept")
            self.assertTrue((paths.realmheart_state/"installed-state.json").is_file())
            receipt=json.loads((paths.realmheart_state/"installed-state.json").read_text())
            self.assertEqual(receipt["install_health"],"healthy")
            self.assertEqual(receipt["activation_state"],"active")
            self.assertEqual(receipt["runtime_health"],"healthy")
            self.assertEqual((paths.config_home/"fish/config.fish").read_text(),"set -gx PERSONAL yes\n")
            self.assertEqual((paths.config_home/"hypr/custom/keybinds.lua").read_text(),"-- MY CUSTOM KEYBINDS\n")
            kitty=(paths.config_home/"kitty/kitty.conf").read_text()
            self.assertIn("font_size 13",kitty); self.assertEqual(kitty.count("BEGIN Realmheart Terminal Theme"),1)
            self.assertTrue((root/"prefix/bin/realmheart").is_file())
            self.assertTrue(paths.baseline_backup.is_dir())

    def test_full_fake_root_failed_essential_probe_rolls_back_exactly_and_keeps_no_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths,snapshot,registry,plan,build,runner,context=self._fixture(root,fail_event_ping=True)
            old_hypr=fingerprint_path(paths.config_home/"hypr")
            old_kitty=(paths.config_home/"kitty/kitty.conf").read_bytes()
            old_fish=(paths.config_home/"fish/config.fish").read_bytes()
            with patch("realmheart_installer.live.orchestrator.NativeBuildExecutor") as build_cls:
                build_cls.return_value.run.return_value=build
                result=LiveInstallExecutor(
                    plan=plan,registry=registry,context=context,paths=paths,source_root=_bootstrap.REPO_ROOT,runner=runner,snapshot=snapshot,
                    decision_selector=lambda decision: FinalAction.RESTORE_PREVIOUS,package_actions_applied=True,allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(),privileged_gid=os.getgid(),persist_diagnostic_on_failure=False,
                ).run()
            self.assertEqual(result.finalization.exit_code,21)
            self.assertEqual(result.finalization.disposition,"rolled_back")
            self.assertEqual(fingerprint_path(paths.config_home/"hypr"),old_hypr)
            self.assertEqual((paths.config_home/"kitty/kitty.conf").read_bytes(),old_kitty)
            self.assertEqual((paths.config_home/"fish/config.fish").read_bytes(),old_fish)
            self.assertFalse((root/"prefix/bin/realmheart").exists())
            self.assertFalse((paths.realmheart_state/"installed-state.json").exists())
            self.assertTrue(paths.baseline_backup.is_dir(),"permanent baseline must survive rollback")


    def test_doctor_failure_is_indeterminate_not_a_new_install_failure(self):
        from realmheart_doctor.acceptance import DoctorAcceptanceError
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths,snapshot,registry,plan,build,runner,context=self._fixture(root)
            selected_decisions=[]
            def select_final(decision):
                selected_decisions.append(decision)
                return FinalAction.KEEP
            with patch("realmheart_installer.live.orchestrator.NativeBuildExecutor") as build_cls, \
                 patch("realmheart_installer.live.orchestrator.assess_candidate_install", side_effect=DoctorAcceptanceError("synthetic doctor failure")):
                build_cls.return_value.run.return_value=build
                result=LiveInstallExecutor(
                    plan=plan,registry=registry,context=context,paths=paths,source_root=_bootstrap.REPO_ROOT,runner=runner,snapshot=snapshot,
                    decision_selector=select_final,package_actions_applied=True,allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(),privileged_gid=os.getgid(),persist_diagnostic_on_failure=False,
                ).run()
            self.assertEqual(result.finalization.disposition,"kept")
            self.assertEqual(result.doctor_assessment.recommendation.value,"indeterminate")
            self.assertEqual(len(selected_decisions),1)
            self.assertTrue(selected_decisions[0].requires_explicit_choice)
            self.assertIsNone(selected_decisions[0].default_action)
            receipt=json.loads((paths.realmheart_state/"installed-state.json").read_text())
            self.assertEqual(receipt["doctor_acceptance"]["recommendation"],"indeterminate")

    def test_doctor_revert_recommendation_can_drive_transaction_rollback(self):
        from realmheart_doctor import AcceptanceAssessment, AcceptanceRecommendation
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); paths,snapshot,registry,plan,build,runner,context=self._fixture(root)
            doctor=AcceptanceAssessment(
                1,AcceptanceRecommendation.REVERT_RECOMMENDED,plan.transaction_id,plan.target_version,plan.manifest_digest,
                1,1,"active","healthy",(),0,0,"synthetic critical Doctor evidence",
            )
            with patch("realmheart_installer.live.orchestrator.NativeBuildExecutor") as build_cls, \
                 patch("realmheart_installer.live.orchestrator.assess_candidate_install", return_value=doctor):
                build_cls.return_value.run.return_value=build
                result=LiveInstallExecutor(
                    plan=plan,registry=registry,context=context,paths=paths,source_root=_bootstrap.REPO_ROOT,runner=runner,snapshot=snapshot,
                    decision_selector=lambda decision: FinalAction.RESTORE_PREVIOUS,package_actions_applied=True,allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(),privileged_gid=os.getgid(),persist_diagnostic_on_failure=False,
                ).run()
            self.assertEqual(result.finalization.disposition,"rolled_back")
            self.assertFalse((paths.realmheart_state/"installed-state.json").exists())

if __name__=="__main__": unittest.main()
