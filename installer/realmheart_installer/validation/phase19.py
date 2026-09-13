"""Phase-19 real-system validation matrix and evidence ledger.

The installer test suite proves transactional mechanics with isolated roots.  Phase
19 adds a release-validation layer: every real-system scenario has a stable ID,
a deterministic fixture counterpart, explicit disposable-host prerequisites, and
an evidence record.  This module intentionally does *not* execute destructive
live installation automatically.  Real-system mutation belongs on a disposable
user/VM/snapshot and requires an operator to follow the printed recipe.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import tempfile
import sys
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Iterable, Mapping, Sequence


PHASE19_SCHEMA_VERSION = 1


class ValidationStatus(str, Enum):
    PENDING = "pending"
    PASS = "pass"
    FAIL = "fail"
    BLOCKED = "blocked"
    SKIPPED = "skipped"


@dataclass(frozen=True)
class ValidationScenario:
    scenario_id: str
    title: str
    purpose: str
    fixture_tests: tuple[str, ...]
    setup: tuple[str, ...]
    acceptance: tuple[str, ...]
    destructive: bool = True


@dataclass
class ScenarioResult:
    scenario_id: str
    status: ValidationStatus = ValidationStatus.PENDING
    recorded_at: str | None = None
    note: str | None = None
    evidence_files: list[dict[str, str | int]] = field(default_factory=list)
    fixture_status: ValidationStatus = ValidationStatus.PENDING
    fixture_duration_seconds: float | None = None
    fixture_output_sha256: str | None = None
    host: dict[str, str] = field(default_factory=dict)
    validated_source_root: str | None = None
    source_revision: str | None = None
    source_dirty: bool | None = None


@dataclass
class Phase19Report:
    schema_version: int
    phase: int
    created_at: str
    updated_at: str
    source_root: str
    source_revision: str | None
    source_dirty: bool | None
    host: dict[str, str]
    scenarios: dict[str, ScenarioResult]
    host_audit: dict[str, object] = field(default_factory=dict)
    fixture_run: dict[str, object] = field(default_factory=dict)


# The fixture tests are not substitutes for the live scenarios.  They are a
# stable release gate proving the same ownership/planning/recovery contracts
# before a disposable real machine is touched.
PHASE19_SCENARIOS: tuple[ValidationScenario, ...] = (
    ValidationScenario(
        "clean-user", "Clean user / fresh install",
        "Prove first managed install creates safety state, installs the complete product, and keeps a valid receipt.",
        (
            "tests.installer.test_installation_detection.InstallationDetectionTests.test_fresh_install_when_no_existing_evidence",
            "tests.installer.test_planning.PlanningTests.test_fresh_install_plans_permanent_baseline",
            "tests.installer.test_live_install.Phase16LiveInstallTests.test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state",
        ),
        (
            "Reset a disposable VM/snapshot or create a genuinely clean disposable user.",
            "Ensure no managed Realmheart receipt/baseline exists for that account.",
            "Run --dry-run install first; only continue if the plan is READY.",
            "Run the normal live installer from this exact source revision.",
        ),
        (
            "Installation is kept healthy or activation-pending only for a justified fresh-session reason.",
            "Permanent baseline exists and is valid.",
            "installed-state.json records the accepted observed state.",
            "recovery-list is clean after completion.",
        ),
    ),
    ValidationScenario(
        "custom-hypr", "Existing custom Hyprland configuration",
        "Prove full-tree takeover preserves the user-owned hypr/custom island and does not clobber drift after planning.",
        (
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_custom_xdg_full_integration_preserves_user_state_and_generates_theme",
            "tests.installer.test_staging.StagingTests.test_custom_island_is_preserved_and_new_defaults_are_seeded",
            "tests.installer.test_phase18_chaos.Phase18ChaosTests.test_active_tree_mutation_between_plan_and_takeover_is_preserved_and_reported",
        ),
        (
            "On a disposable snapshot, create distinctive files under ~/.config/hypr/custom/ plus unrelated old Hyprland files.",
            "Capture checksums of custom/ before installation.",
            "Run a fresh install or upgrade and then compare custom/ byte-for-byte.",
        ),
        (
            "All pre-existing custom/ files survive byte-for-byte.",
            "Realmheart defaults replace the managed Hyprland tree as planned.",
            "A deliberate post-plan edit is refused rather than silently overwritten.",
        ),
    ),
    ValidationScenario(
        "custom-kitty", "Heavily customized Kitty configuration",
        "Prove Realmheart owns only its managed include/drop-in and preserves unrelated kitty.conf content.",
        (
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_custom_xdg_full_integration_preserves_user_state_and_generates_theme",
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_reinstall_is_idempotent_for_kitty_and_fish_personal_config",
            "tests.installer.test_managed_block.ManagedBlockTests.test_remove_preserves_unrelated_current_bytes",
        ),
        (
            "Create a heavily customized kitty.conf with distinctive comments, mappings, includes, and whitespace.",
            "Hash/copy the file before install, then install, reinstall, and finally uninstall with keep-current.",
        ),
        (
            "Exactly one Realmheart managed block exists while installed.",
            "All bytes outside Realmheart markers remain intact.",
            "Uninstall removes only the Realmheart block/drop-in.",
        ),
    ),
    ValidationScenario(
        "custom-fish", "Heavily customized Fish configuration",
        "Prove personal config.fish is not an installer mutation target.",
        (
            "tests.installer.test_terminal_contract.TerminalContractTests.test_fish_dropins_do_not_touch_config_fish",
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_custom_xdg_full_integration_preserves_user_state_and_generates_theme",
            "tests.installer.test_verification_engine.Phase13VerificationTests.test_personal_fish_config_drift_is_detected_as_terminal_failure",
        ),
        (
            "Create a distinctive ~/.config/fish/config.fish and record its SHA-256.",
            "Install, reinstall, and uninstall Realmheart in the disposable environment.",
        ),
        (
            "config.fish SHA-256 remains identical throughout normal integration/uninstall.",
            "Realmheart Fish integration exists only in conf.d drop-ins.",
        ),
    ),
    ValidationScenario(
        "non-default-xdg", "Non-default XDG roots",
        "Prove all user-facing paths render from resolved XDG_CONFIG_HOME/XDG_STATE_HOME rather than hidden ~/.config assumptions.",
        (
            "tests.installer.test_context.ContextTests.test_non_default_xdg_roots_are_honored",
            "tests.installer.test_fake_home.FakeHomeTests.test_fake_home_and_non_default_xdg_are_isolated",
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_custom_xdg_full_integration_preserves_user_state_and_generates_theme",
        ),
        (
            "Use a disposable account/session with XDG_CONFIG_HOME and XDG_STATE_HOME set to non-default absolute paths before launching Realmheart.",
            "Run --dry-run install and inspect rendered paths before live mutation.",
        ),
        (
            "Managed config, state, receipt, generated terminal state, and user units use the resolved XDG roots.",
            "No accidental Realmheart config is created in default ~/.config or ~/.local/state paths.",
        ),
    ),
    ValidationScenario(
        "conflicting-owned-files", "Pre-existing same-name Realmheart files/units",
        "Prove exact preimages are preserved and restored instead of assuming same-name files were created by Realmheart.",
        (
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_rollback_is_surgical_and_restores_preexisting_same_name_files",
            "tests.installer.test_uninstall.Phase17UninstallTests.test_keep_current_removes_realmheart_integration_but_preserves_user_config_and_history",
        ),
        (
            "Before install, create distinctive same-name Realmheart terminal drop-ins/user-unit files at declared targets.",
            "Record bytes/modes, install Realmheart, then perform rollback/uninstall on separate restored snapshots.",
        ),
        (
            "Pre-existing same-name bytes are recoverable from installer safety state.",
            "Rollback restores those files exactly when Realmheart replaced them.",
            "No containing directory is recursively replaced or deleted.",
        ),
    ),
    ValidationScenario(
        "upgrade", "Previous Realmheart version / upgrade",
        "Prove an older managed install upgrades transactionally with previous-version recovery state.",
        (
            "tests.installer.test_installation_detection.InstallationDetectionTests.test_managed_receipt_drives_upgrade_mode",
            "tests.installer.test_finalization.Phase16FinalizationTests.test_rollback_preserves_previous_receipt",
        ),
        (
            "Restore a disposable snapshot containing an older *managed-installer* Realmheart release and its valid receipt/baseline.",
            "Run the newer source dry-run and confirm Mode=UPGRADE before mutation.",
            "Perform the upgrade; use a second snapshot to inject a critical failure and exercise rollback.",
        ),
        (
            "Successful upgrade retains permanent baseline and records new receipt.",
            "Failed upgrade restores previous managed state/receipt rather than publishing the failed target receipt.",
        ),
    ),
    ValidationScenario(
        "reinstall", "Same-version reinstall",
        "Prove reinstall is idempotent and does not spam backups or duplicate managed integration.",
        (
            "tests.installer.test_installation_detection.InstallationDetectionTests.test_binary_version_beats_source_inference",
            "tests.installer.test_configuration_integration.ConfigurationIntegrationTests.test_reinstall_is_idempotent_for_kitty_and_fish_personal_config",
        ),
        (
            "Start from a healthy managed installation produced by the same source/release.",
            "Run --dry-run install and confirm Mode=REINSTALL, then perform the reinstall.",
        ),
        (
            "No duplicate Kitty managed block or Fish integration is created.",
            "Permanent baseline identity is unchanged and backup history does not grow without reason.",
            "Final receipt remains internally consistent.",
        ),
    ),
    ValidationScenario(
        "downgrade", "Managed downgrade",
        "Prove newer managed state is recognized and downgrade uses an explicit recoverable previous-version snapshot.",
        (
            "tests.installer.test_installation_detection.InstallationDetectionTests.test_managed_receipt_drives_downgrade_mode",
            "tests.installer.test_planning.PlanningTests.test_target_fingerprint_is_part_of_plan_identity",
        ),
        (
            "Start from a newer managed Realmheart release in a disposable VM snapshot.",
            "Checkout/use the older release source intended for downgrade and run dry-run first.",
            "Confirm the downgrade warning and previous-version recovery plan before live mutation.",
        ),
        (
            "Mode is explicitly DOWNGRADE; it is never mislabeled reinstall/upgrade.",
            "Newer working state has a recoverable snapshot before the older release is applied.",
            "Rollback returns to the newer known state if downgrade verification fails.",
        ),
    ),
    ValidationScenario(
        "missing-soft-dependency", "Missing optional/soft verification dependency",
        "Prove a genuinely soft verification capability can be absent without manufacturing a Core dependency failure.",
        (
            "tests.installer.test_capabilities.CapabilityTests.test_missing_tesseract_language_is_reported",
            "tests.installer.test_component_handlers.Phase12ComponentHandlerTests.test_local_screenshot_failure_blocks_only_dependent_branches",
        ),
        (
            "On a disposable snapshot, remove or mask one manifest-declared soft verification capability (prefer verification.gtest or verification.dbus-run-session).",
            "Do not remove a Core-required build/runtime capability for this scenario.",
            "Run preflight/dry-run and then install if the plan remains supported.",
        ),
        (
            "The missing soft capability is named explicitly.",
            "Unrelated Core components remain runnable/healthy.",
            "The final health/report does not exaggerate the soft gap into an unrelated Core root failure.",
        ),
    ),
    ValidationScenario(
        "multi-monitor", "Multi-monitor topology",
        "Prove real Hyprland monitor topology is captured without changing installation semantics or leaking unnecessary identifiers.",
        (
            "tests.installer.test_environment_detect.DetectTests.test_hyprland_json_version_and_monitors",
            "tests.installer.test_diagnostics.Phase15DiagnosticsTests.test_report_never_serializes_private_environment_or_monitor_description",
        ),
        (
            "Use a disposable supported Hyprland session with at least two active monitors; mixed position/scale is preferred.",
            "Run preflight/dry-run and inspect the captured topology before live installation.",
        ),
        (
            "Monitor count, resolution, refresh, scale, position and focused/primary facts are correct.",
            "Diagnostic output omits unnecessary hardware description/serial-style identifiers.",
            "Install/verification succeeds or fails for a specific non-topology reason.",
        ),
    ),
    ValidationScenario(
        "broken-component", "Intentionally broken component",
        "Prove a real component failure is attributed precisely while independent work continues and final health is honest.",
        (
            "tests.installer.test_component_execution.ComponentExecutionTests.test_failing_component_blocks_dependents_but_not_unrelated_components",
            "tests.installer.test_verification_engine.Phase13VerificationTests.test_copied_but_broken_executable_fails_structural_verification",
            "tests.installer.test_diagnostics.Phase15DiagnosticsTests.test_report_groups_root_failure_and_transitive_blocked_components",
        ),
        (
            "On a disposable snapshot, introduce one controlled breakage with a known expected component/build-unit owner.",
            "Prefer a reversible artifact/probe break rather than corrupting unrelated system state.",
            "Run verification/finalization and retain the generated report.",
        ),
        (
            "The exact component/stage/error is reported as root failure.",
            "Dependents become BLOCKED where appropriate while unrelated components still process.",
            "Criticality and keep/rollback recommendation match the broken component category.",
        ),
    ),
    ValidationScenario(
        "controlled-interrupt", "Ctrl+C at controlled mutation points",
        "Prove a real interrupted process leaves durable WAL/recovery state and safe re-entry behavior.",
        (
            "tests.installer.test_phase18_chaos.Phase18ChaosTests.test_live_keyboard_interrupt_marks_transaction_interrupted_and_emits_report",
            "tests.installer.test_phase18_chaos.Phase18ChaosTests.test_hard_process_death_is_reconstructed_on_next_invocation_and_safely_recovered",
            "tests.installer.test_phase18_chaos.Phase18ChaosTests.test_new_mutating_cli_transaction_is_blocked_by_abandoned_recoverable_transaction",
        ),
        (
            "Use a disposable VM snapshot only; this scenario intentionally interrupts a mutating install.",
            "Interrupt once at a documented safe early point and once after at least one journaled live mutation.",
            "Run recovery-list/recovery-inspect before attempting any new install.",
        ),
        (
            "Interrupted transaction is visible on re-entry with recovery.json/WAL evidence.",
            "A new mutation is blocked until the abandoned transaction is recovered or explicitly acknowledged.",
            "Recovery produces either a proven rollback or explicit manual-attention state; never an unexplained partial install.",
        ),
    ),
)


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _git_identity(source_root: Path) -> tuple[str | None, bool | None]:
    try:
        rev = subprocess.run(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=5, check=False,
        )
        status = subprocess.run(
            ["git", "-C", str(source_root), "status", "--porcelain"],
            capture_output=True, text=True, timeout=5, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None, None
    revision = rev.stdout.strip() if rev.returncode == 0 else None
    dirty = bool(status.stdout.strip()) if status.returncode == 0 else None
    return revision, dirty


def _host_summary() -> dict[str, str]:
    distro = "unknown"
    os_release = Path("/etc/os-release")
    if os_release.is_file():
        values: dict[str, str] = {}
        for line in os_release.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key] = value.strip().strip('"')
        distro = values.get("PRETTY_NAME") or values.get("NAME") or distro
    return {
        "distribution": distro,
        "architecture": platform.machine(),
        "kernel": platform.release(),
        "session_type": os.environ.get("XDG_SESSION_TYPE", "unknown"),
    }


def create_report(source_root: Path) -> Phase19Report:
    source_root = source_root.resolve()
    revision, dirty = _git_identity(source_root)
    now = _utc_now()
    return Phase19Report(
        schema_version=PHASE19_SCHEMA_VERSION,
        phase=19,
        created_at=now,
        updated_at=now,
        source_root=str(source_root),
        source_revision=revision,
        source_dirty=dirty,
        host=_host_summary(),
        scenarios={item.scenario_id: ScenarioResult(item.scenario_id) for item in PHASE19_SCENARIOS},
    )


def _report_to_dict(report: Phase19Report) -> dict[str, object]:
    payload = asdict(report)
    for result in payload["scenarios"].values():
        if isinstance(result.get("status"), ValidationStatus):
            result["status"] = result["status"].value
        if isinstance(result.get("fixture_status"), ValidationStatus):
            result["fixture_status"] = result["fixture_status"].value
    return payload


def save_report(report: Phase19Report, path: Path) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    report.updated_at = _utc_now()
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(_report_to_dict(report), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temp, 0o600)
    os.replace(temp, path)


def load_report(path: Path) -> Phase19Report:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if int(payload.get("schema_version", -1)) != PHASE19_SCHEMA_VERSION or int(payload.get("phase", -1)) != 19:
        raise ValueError("unsupported Phase-19 validation report")
    scenario_ids = {item.scenario_id for item in PHASE19_SCENARIOS}
    raw_scenarios = payload.get("scenarios")
    if not isinstance(raw_scenarios, dict) or set(raw_scenarios) != scenario_ids:
        raise ValueError("Phase-19 validation report scenario set does not match this release")
    scenarios: dict[str, ScenarioResult] = {}
    for scenario_id, raw in raw_scenarios.items():
        scenarios[scenario_id] = ScenarioResult(
            scenario_id=scenario_id,
            status=ValidationStatus(raw.get("status", "pending")),
            recorded_at=raw.get("recorded_at"),
            note=raw.get("note"),
            evidence_files=list(raw.get("evidence_files") or []),
            fixture_status=ValidationStatus(raw.get("fixture_status", "pending")),
            fixture_duration_seconds=raw.get("fixture_duration_seconds"),
            fixture_output_sha256=raw.get("fixture_output_sha256"),
            host=dict(raw.get("host") or {}),
            validated_source_root=raw.get("validated_source_root"),
            source_revision=raw.get("source_revision"),
            source_dirty=raw.get("source_dirty"),
        )
    return Phase19Report(
        schema_version=PHASE19_SCHEMA_VERSION,
        phase=19,
        created_at=str(payload["created_at"]),
        updated_at=str(payload["updated_at"]),
        source_root=str(payload["source_root"]),
        source_revision=payload.get("source_revision"),
        source_dirty=payload.get("source_dirty"),
        host=dict(payload.get("host") or {}),
        scenarios=scenarios,
        host_audit=dict(payload.get("host_audit") or {}),
        fixture_run=dict(payload.get("fixture_run") or {}),
    )


def _fsync_works(directory: Path) -> bool:
    try:
        directory.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix="realmheart-phase19-fsync-", dir=directory)
        try:
            os.write(fd, b"probe")
            os.fsync(fd)
        finally:
            os.close(fd)
            Path(name).unlink(missing_ok=True)
        return True
    except OSError:
        return False


def _select_fixture_tmpdir() -> Path | None:
    override = os.environ.get("REALMHEART_VALIDATION_TMPDIR")
    candidates = []
    if override:
        candidates.append(Path(override).expanduser())
    candidates.extend((Path(tempfile.gettempdir()), Path("/dev/shm")))
    seen: set[str] = set()
    for candidate in candidates:
        try:
            resolved = str(candidate.resolve())
        except OSError:
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        if candidate.is_dir() and os.access(candidate, os.W_OK | os.X_OK) and _fsync_works(candidate):
            return candidate.resolve()
    return None


def _run(command: Sequence[str], *, cwd: Path, env: Mapping[str, str] | None = None, timeout: int = 180) -> tuple[int, bytes, float]:
    started = time.monotonic()
    kwargs = {
        "cwd": cwd,
        "env": dict(env) if env is not None else None,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT,
    }
    if os.name == "posix":
        kwargs["start_new_session"] = True
    try:
        proc = subprocess.Popen(list(command), **kwargs)
    except OSError as exc:
        return 127, f"failed to execute: {exc}\n".encode(), time.monotonic() - started
    try:
        output, _ = proc.communicate(timeout=timeout)
        return proc.returncode, output or b"", time.monotonic() - started
    except subprocess.TimeoutExpired as exc:
        if os.name == "posix":
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        else:
            proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
                proc.wait(timeout=5)
            except (subprocess.TimeoutExpired, ProcessLookupError):
                pass
        if proc.stdout is not None:
            try:
                proc.stdout.close()
            except OSError:
                pass
        prefix = exc.output or b""
        return 124, prefix + b"\n[phase19 validation timeout]\n", time.monotonic() - started


def run_fixture_matrix(
    report: Phase19Report, *, source_root: Path, timeout: int = 180, evidence_dir: Path | None = None,
) -> list[tuple[str, ValidationStatus, float]]:
    source_root = source_root.resolve()
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join((str(source_root / "installer"), str(source_root), env.get("PYTHONPATH", ""))).rstrip(os.pathsep)
    fixture_tmpdir = _select_fixture_tmpdir()
    if fixture_tmpdir is not None:
        for key in ("TMPDIR", "TMP", "TEMP"):
            env[key] = str(fixture_tmpdir)

    # Several matrix rows share the same integration invariant. Run one bounded
    # unittest process containing every unique target instead of paying process
    # startup/teardown cost for each row. If the corpus is fully green every row
    # is green; on failure, parse unittest's verbose per-test status and fail
    # conservatively for any target whose PASS cannot be proven.
    targets: list[str] = []
    seen: set[str] = set()
    for scenario in PHASE19_SCENARIOS:
        for target in scenario.fixture_tests:
            if target not in seen:
                seen.add(target)
                targets.append(target)

    code, output, duration = _run(
        [sys.executable, "-m", "unittest", "-v", *targets],
        cwd=source_root, env=env, timeout=timeout,
    )
    target_status: dict[str, ValidationStatus] = {}
    if code == 0:
        target_status = {target: ValidationStatus.PASS for target in targets}
    else:
        text = output.decode("utf-8", errors="replace")
        for target in targets:
            short = target.rsplit(".", 1)[-1]
            # Verbose unittest output always contains the method name followed by
            # the fully qualified test id and terminal status. Avoid pretending a
            # test passed unless an explicit `... ok` line is present.
            passed = any(short in line and line.rstrip().endswith("... ok") for line in text.splitlines())
            target_status[target] = ValidationStatus.PASS if passed else ValidationStatus.FAIL

    corpus_sha = _sha256_bytes(output)
    fixture_run: dict[str, object] = {
        "run_at": _utc_now(),
        "status": "pass" if code == 0 else "fail",
        "returncode": code,
        "duration_seconds": round(duration, 3),
        "test_count": len(targets),
        "output_sha256": corpus_sha,
    }
    if evidence_dir is not None:
        fixture_run["evidence"] = _write_private_log(evidence_dir.resolve() / "fixture-matrix", "fixture-matrix.log", output)
    report.fixture_run = fixture_run
    results: list[tuple[str, ValidationStatus, float]] = []
    for scenario in PHASE19_SCENARIOS:
        status = ValidationStatus.PASS if all(target_status[target] is ValidationStatus.PASS for target in scenario.fixture_tests) else ValidationStatus.FAIL
        stored = report.scenarios[scenario.scenario_id]
        stored.fixture_status = status
        stored.fixture_duration_seconds = round(duration, 3)
        # One corpus hash intentionally ties every row to the exact same fixture
        # execution rather than implying thirteen independent runs occurred.
        stored.fixture_output_sha256 = corpus_sha
        results.append((scenario.scenario_id, status, duration))
    return results


def _write_private_log(directory: Path, name: str, output: bytes) -> dict[str, object]:
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    path = directory / name
    path.write_bytes(output)
    os.chmod(path, 0o600)
    return {"file": path.name, "sha256": _sha256_bytes(output), "bytes": len(output)}


def run_host_audit(report: Phase19Report, *, source_root: Path, evidence_dir: Path, timeout: int = 180) -> bool:
    """Run read-only release gates on the current supported host.

    Raw command output is private evidence on disk.  The JSON ledger stores only
    return codes, duration and hashes so it does not become an accidental copy of
    absolute HOME paths or verbose environment diagnostics.
    """

    source_root = source_root.resolve()
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join((str(source_root / "installer"), str(source_root), env.get("PYTHONPATH", ""))).rstrip(os.pathsep)
    commands = (
        ("manifest", [sys.executable, "tools/validate-realmheart-manifest.py"], {0}),
        ("install-dry-run", [sys.executable, "installer/realmheart_installer.py", "--dry-run", "install"], {0}),
        ("recovery-list", [sys.executable, "installer/realmheart_installer.py", "recovery-list"], {0}),
    )
    audit: dict[str, object] = {"run_at": _utc_now(), "checks": {}}
    all_pass = True
    log_dir = evidence_dir.resolve() / "host-audit"
    for name, command, accepted in commands:
        code, output, duration = _run(command, cwd=source_root, env=env, timeout=timeout)
        evidence = _write_private_log(log_dir, f"{name}.log", output)
        passed = code in accepted
        all_pass = all_pass and passed
        audit["checks"][name] = {
            "status": "pass" if passed else "fail",
            "returncode": code,
            "duration_seconds": round(duration, 3),
            "evidence": evidence,
        }
    report.host_audit = audit
    return all_pass


def record_result(
    report: Phase19Report,
    *,
    scenario_id: str,
    status: ValidationStatus,
    note: str | None = None,
    evidence_paths: Iterable[Path] = (),
    evidence_dir: Path | None = None,
    validated_source_root: Path | None = None,
) -> ScenarioResult:
    if scenario_id not in report.scenarios:
        raise ValueError(f"unknown Phase-19 scenario: {scenario_id}")
    if status is ValidationStatus.PENDING:
        raise ValueError("record-result cannot set a scenario back to pending")
    result = report.scenarios[scenario_id]
    evidence: list[dict[str, str | int]] = []
    for source in evidence_paths:
        original = Path(source)
        if original.is_symlink() or not original.is_file():
            raise ValueError(f"evidence must be a regular non-symlink file: {original}")
        source = original.resolve()
        item: dict[str, str | int] = {
            "name": source.name,
            "sha256": _sha256_file(source),
            "bytes": source.stat().st_size,
        }
        if evidence_dir is not None:
            target_dir = evidence_dir.resolve() / scenario_id
            target_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
            target = target_dir / source.name
            if target.exists():
                target = target_dir / f"{source.stem}-{item['sha256'][:12]}{source.suffix}"
            shutil.copyfile(source, target)
            os.chmod(target, 0o600)
            item["stored_as"] = str(target.relative_to(evidence_dir.resolve()))
        evidence.append(item)
    result.status = status
    result.recorded_at = _utc_now()
    result.note = note
    result.evidence_files = evidence
    result.host = _host_summary()
    source_root = (validated_source_root or Path(report.source_root)).resolve()
    revision, dirty = _git_identity(source_root)
    result.validated_source_root = str(source_root)
    result.source_revision = revision
    result.source_dirty = dirty
    return result


def summarize_report(report: Phase19Report) -> dict[str, object]:
    counts = {status.value: 0 for status in ValidationStatus}
    fixture_counts = {status.value: 0 for status in ValidationStatus}
    for result in report.scenarios.values():
        counts[result.status.value] += 1
        fixture_counts[result.fixture_status.value] += 1
    live_complete = counts[ValidationStatus.PASS.value] == len(PHASE19_SCENARIOS)
    fixture_complete = fixture_counts[ValidationStatus.PASS.value] == len(PHASE19_SCENARIOS)
    host_checks = report.host_audit.get("checks", {}) if isinstance(report.host_audit, dict) else {}
    host_pass = bool(host_checks) and all(
        isinstance(item, dict) and item.get("status") == "pass" for item in host_checks.values()
    )
    return {
        "scenario_count": len(PHASE19_SCENARIOS),
        "live": counts,
        "fixture": fixture_counts,
        "fixture_complete": fixture_complete,
        "host_audit_pass": host_pass,
        "phase19_complete": live_complete and fixture_complete and host_pass,
    }
