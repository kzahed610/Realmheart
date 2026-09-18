"""Doctor-owned repair runners: bounded, structured, consent-gated.

Every runner executes one structured argv, never a shell string, and never
runs without explicit consent from the orchestrator.  Automatic paths (boot,
post-update) do not import this module.  A successful command is never accepted
as a successful repair: the orchestrator verifies with a fresh diagnosis.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from realmheart_maintenance.packages import query_installed_packages

from .repair import (
    ACTION_INSTALL_PACKAGE,
    ACTION_POST_CHECKS,
    ACTION_REBUILD,
    ACTION_RESTART_SERVICE,
    RISK_SAFE,
    RepairAction,
    RepairPlan,
)


@dataclass(frozen=True)
class RunnerOutcome:
    status: str  # succeeded | failed | refused | unavailable
    detail: str


def _default_run(argv: tuple[str, ...], timeout: float) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout, check=False)


class PackageInstallRunner:
    """Install resolved packages with ``sudo pacman -S --needed`` and re-query."""

    def __init__(self, *, runner=None, sudo: str | None = None, pacman: str | None = None,
                 timeout: float = 600.0, stdin_isatty=None) -> None:
        self.runner = runner or _default_run
        self.sudo = sudo or shutil.which("sudo") or "sudo"
        self.pacman = pacman or shutil.which("pacman") or "pacman"
        self.timeout = timeout
        self.stdin_isatty = stdin_isatty or (lambda: sys.stdin.isatty())

    def __call__(self, action: RepairAction) -> RunnerOutcome:
        packages = tuple(action.packages)
        if not packages:
            return RunnerOutcome("unavailable", "no packages were resolved for this repair")
        if not self.stdin_isatty():
            return RunnerOutcome("refused", "package installation needs an interactive terminal for sudo")
        try:
            completed = self.runner((self.sudo, self.pacman, "-S", "--needed", *packages), self.timeout)
        except (OSError, subprocess.SubprocessError) as exc:
            return RunnerOutcome("failed", f"package installation could not run: {type(exc).__name__}")
        if completed.returncode != 0:
            return RunnerOutcome("failed", f"pacman exited with status {completed.returncode}")
        verification = query_installed_packages(
            packages, runner=lambda argv, timeout: self.runner(argv, timeout)
        )
        missing = [item.package for item in verification if not item.installed]
        if missing:
            return RunnerOutcome("failed", "pacman reported success but these are still missing: " + ", ".join(missing))
        return RunnerOutcome("succeeded", "installed: " + ", ".join(packages))


class RebuildRunner:
    """Rebuild one component's targets and install only that component."""

    def __init__(self, *, component_id: str, build_dir: str | None = None, prefix: str | None = None,
                 runner=None, cmake: str | None = None, timeout: float = 1200.0,
                 installer_bound: bool = False) -> None:
        self.component_id = component_id
        self.build_dir = build_dir
        self.prefix = prefix
        self.runner = runner or _default_run
        self.cmake = cmake or shutil.which("cmake") or "cmake"
        self.timeout = timeout
        self.installer_bound = installer_bound

    def __call__(self, action: RepairAction) -> RunnerOutcome:
        if self.installer_bound:
            return RunnerOutcome(
                "refused",
                "this component owns installer-bound artifacts; reinstall it with the Realmheart installer",
            )
        if not self.build_dir or not Path(self.build_dir).is_dir():
            return RunnerOutcome(
                "unavailable",
                "build directory unknown; pass --build-dir or reinstall with the Realmheart installer",
            )
        if not self.prefix:
            return RunnerOutcome("unavailable", "install prefix unknown; pass --prefix")
        targets = tuple(item for item in action.targets if item)
        if not targets:
            return RunnerOutcome("unavailable", "no build targets are declared for this component")
        try:
            build = self.runner((self.cmake, "--build", str(self.build_dir), "--target", *targets), self.timeout)
        except (OSError, subprocess.SubprocessError) as exc:
            return RunnerOutcome("failed", f"build could not run: {type(exc).__name__}")
        if build.returncode != 0:
            return RunnerOutcome("failed", f"build failed with status {build.returncode}")
        try:
            install = self.runner(
                (self.cmake, "--install", str(self.build_dir), "--component", self.component_id,
                 "--prefix", str(self.prefix)),
                self.timeout,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            return RunnerOutcome("failed", f"component install could not run: {type(exc).__name__}")
        if install.returncode != 0:
            return RunnerOutcome("failed", f"component install failed with status {install.returncode}")
        return RunnerOutcome("succeeded", "rebuilt and reinstalled: " + ", ".join(targets))


class ServiceRestartRunner:
    """Restart the component's systemd user units."""

    def __init__(self, *, runner=None, systemctl: str | None = None, timeout: float = 60.0) -> None:
        self.runner = runner or _default_run
        self.systemctl = systemctl or shutil.which("systemctl") or "systemctl"
        self.timeout = timeout

    def __call__(self, action: RepairAction) -> RunnerOutcome:
        units = tuple(item for item in action.targets if item)
        if not units:
            return RunnerOutcome("unavailable", "no user services are declared for this component")
        failed: list[str] = []
        for unit in units:
            try:
                completed = self.runner((self.systemctl, "--user", "restart", unit), self.timeout)
            except (OSError, subprocess.SubprocessError):
                failed.append(unit)
                continue
            if completed.returncode != 0:
                failed.append(unit)
        if failed:
            return RunnerOutcome("failed", "these user services did not restart: " + ", ".join(failed))
        return RunnerOutcome("succeeded", "restarted: " + ", ".join(units))


def default_repair_runners(
    *,
    component_id: str,
    build_dir: str | None = None,
    prefix: str | None = None,
    installer_bound: bool = False,
    runner=None,
) -> dict[str, object]:
    return {
        ACTION_INSTALL_PACKAGE: PackageInstallRunner(runner=runner),
        ACTION_REBUILD: RebuildRunner(
            component_id=component_id, build_dir=build_dir, prefix=prefix,
            installer_bound=installer_bound, runner=runner,
        ),
        ACTION_RESTART_SERVICE: ServiceRestartRunner(runner=runner),
    }


@dataclass(frozen=True)
class RepairExecution:
    action_type: str
    risk: str
    fingerprint: str
    status: str
    detail: str | None = None
    verified: bool | None = None

    def to_dict(self) -> dict[str, object]:
        return {"action_type": self.action_type, "risk": self.risk, "fingerprint": self.fingerprint,
                "status": self.status, "detail": self.detail, "verified": self.verified}


@dataclass(frozen=True)
class RepairReport:
    component_id: str
    executions: tuple[RepairExecution, ...]
    verified: bool
    component_status: str | None

    def to_dict(self) -> dict[str, object]:
        return {"component_id": self.component_id, "verified": self.verified,
                "component_status": self.component_status,
                "executions": [item.to_dict() for item in self.executions]}


def render_repair_plan(plan: RepairPlan) -> str:
    lines = [f"Repair plan for {plan.component_id} ({plan.reason})"]
    for index, action in enumerate(plan.actions, start=1):
        target = ""
        if action.packages:
            target = " [" + ", ".join(action.packages) + "]"
        elif action.targets:
            target = " [" + ", ".join(action.targets) + "]"
        lines.append(f"  {index}. [{action.risk}] {action.description}{target}")
    lines.extend(f"  note: {note}" for note in plan.notes)
    return "\n".join(lines)


def render_repair_report(report: RepairReport) -> str:
    outcome = "verified healthy" if report.verified else (report.component_status or "unverified")
    lines = [f"Repair for {report.component_id}: {outcome}"]
    for item in report.executions:
        detail = f" — {item.detail}" if item.detail else ""
        lines.append(f"  {item.action_type} [{item.risk}] {item.status}{detail}")
    return "\n".join(lines)


def run_repair_plan(
    plan: RepairPlan,
    *,
    consent,
    runners=None,
    verifier=None,
    attempted: tuple[str, ...] = (),
) -> RepairReport:
    """Execute one consented plan and verify the outcome with fresh evidence.

    ``consent`` is consulted for every non-SAFE action; ``verifier`` performs
    the post-repair diagnosis and returns ``(component_status, detail)``.
    A failed or unavailable action stops further non-SAFE work, but the
    post-repair checks always run so the true state is recorded.
    """

    runners = runners or {}
    attempted_set = set(attempted)
    executions: list[RepairExecution] = []
    verified = False
    component_status: str | None = None
    stopped = False
    for action in plan.actions:
        fingerprint = action.fingerprint()
        if action.action_type == ACTION_POST_CHECKS:
            if verifier is None:
                executions.append(RepairExecution(action.action_type, action.risk, fingerprint,
                                                  "unavailable", "no verification callback was provided"))
                continue
            status, detail = verifier()
            component_status = status
            verified = status == "healthy"
            executions.append(RepairExecution(action.action_type, action.risk, fingerprint,
                                              "verified" if verified else "unverified", detail, verified))
            continue
        if fingerprint in attempted_set:
            executions.append(RepairExecution(
                action.action_type, action.risk, fingerprint, "already_attempted",
                "this exact repair was already attempted for the open incident",
            ))
            continue
        if stopped:
            executions.append(RepairExecution(action.action_type, action.risk, fingerprint,
                                              "skipped_after_failure", "an earlier repair step failed"))
            continue
        if action.risk != RISK_SAFE and not consent(action):
            executions.append(RepairExecution(action.action_type, action.risk, fingerprint,
                                              "skipped_no_consent", "consent was not granted"))
            continue
        runner = runners.get(action.action_type)
        if runner is None:
            executions.append(RepairExecution(action.action_type, action.risk, fingerprint,
                                              "unavailable", "no runner is available for this action"))
            stopped = True
            continue
        outcome = runner(action)
        executions.append(RepairExecution(action.action_type, action.risk, fingerprint,
                                          outcome.status, outcome.detail))
        if outcome.status != "succeeded":
            stopped = True
    return RepairReport(plan.component_id, tuple(executions), verified, component_status)
